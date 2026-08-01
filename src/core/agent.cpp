#include "agent.h"
#include "prompts.h"
#include "json.hpp"
#include "debug_log.h"
#include <cstdio>
#include <algorithm>
#include <iterator>
#include <sstream>
#include <set>
#include <unordered_map>

// ============================================================================
// Templates & error strings
// ============================================================================
static constexpr const char* TOOL_PATHS_HAVE = "[TOOL PATHS] Pre-configured tools -- use these:\n";
static constexpr const char* TOOL_PATHS_NONE = "[TOOL PATHS] No pre-configured tools.\n";
static constexpr const char* CONTEXT_WARNING_TEMPLATE =
    "Context is getting full (~%zuK/%zuK). Keep responses very concise.";
static constexpr const char* ERROR_CONSECUTIVE_FAILURES =
    "Multiple tool call errors. Give your final answer now. Do not use more tools.";
static constexpr const char* ERROR_STUCK_LOOP_PREFIX =
    "\xe2\x9a\xa0 [Auto-recovery] You called '";
static constexpr const char* ERROR_STUCK_LOOP_MIDDLE =
    "' repeatedly and it failed ";
static constexpr const char* ERROR_STUCK_LOOP_SUFFIX =
    " times.\n\nTool calls cleaned up, last result preserved. Continue differently.";
static constexpr const char* BATCH_COMPLETE =
    "[Batch complete. Continuing...]";
static constexpr const char* HTTP_RETRY_FAILED =
    "Error: API request failed after retry.\nError: ";
static constexpr const char* USER_REJECTED_PREFIX =
    "[User rejected the following tool call(s): ";
static constexpr const char* WS_VIOLATION_GUIDANCE_NO_WS =
    "(not set -- defaults to exe dir)";
static constexpr const char* QUICK_HELP =
    "### Quick Commands\n\n| Command | Description |\n|---------|-------------|\n"
    "| `/clear` | Clear session |\n| `/help`  | Show this help |\n"
    "| `/save`  | Save session |\n| `/load`  | Load session |\n"
    "| `/compress` | Summarize early messages |\n";
static constexpr const char* SESSION_CLEARED = "Session cleared.";
static constexpr const char* SAVE_TRIGGERED = "Save triggered.";
static constexpr const char* LOAD_TRIGGERED = "Load triggered.";

static std::string buildToolPathsMessage(const std::string& cpp, const std::string& py) {
    if (cpp.empty() && py.empty()) return TOOL_PATHS_NONE;
    std::string msg = TOOL_PATHS_HAVE;
    if (!cpp.empty()) msg += "- C++ compiler: " + cpp + "\n";
    if (!py.empty())  msg += "- Python: " + py + "\n";
    return msg;
}

// ============================================================================
// Agent
// ============================================================================

Agent::Agent(DeepSeekClient& client, ToolRegistry& tools)
    : client(client), toolRegistry(tools)
{
    registerTodoTool(tools, &todoData, &todoMutex);
    contextBudget = Session::computeContextBudget(effectiveContextWindow(), configMaxTokens);
}

Agent::~Agent() { cancel(); }

void Agent::addPersistedMessage(const Message& msg) {
    session.addMessage(msg);
    contextTokensDirty_ = true;
    if (storage && storage->isOpen()) {
        Message copy = msg;
        auto write = [st = storage, c = std::move(copy)]() mutable { st->insertMessage(c); };
        if (enqueueWrite) enqueueWrite(std::move(write));
        else write();
    }
}

void Agent::loadFromStorage() {
    if (!storage || !storage->isOpen()) return;
    session.loadMessages(storage->queryMessages(0));
    contextTokensDirty_ = true;
}

// ========== Lifecycle: startTurn + run ==========

void Agent::startTurn(const std::string& text) {
    debugLogf("[Agent] startTurn: \"%s\"", truncateForLog(text, 200).c_str());
    if (phase_.load() != AgentPhase::Idle) { cancel(); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    if (ensureStorage) ensureStorage();
    { std::lock_guard<std::mutex> lk(todoMutex); todoData.incomingUserPrompt = text; }
    cancelRequested_.store(false, std::memory_order_release);
    hasUserInput_ = true;
    queuedUserText_ = text;
    approvalDone_ = false;
}

void Agent::run() {
    debugLog("[Agent] run: BEGIN");
    if (hasUserInput_) { addPersistedMessage(Message::User(queuedUserText_)); hasUserInput_ = false; }
    doCompaction();
    checkContextWarning();
    toolCallDepth_ = 0;
    reasoningLengthRetries_ = 0;
    phase_ = AgentPhase::Streaming;
    updateSnapshot();

    while (phase_ != AgentPhase::Idle && !cancelRequested_.load()) {
        switch (phase_.load()) {

        case AgentPhase::Streaming: {
            updateSnapshot();
            ChatRequest req = buildChatRequest();
            currentContent_.clear(); currentReasoning_.clear();
            currentToolCalls_.clear(); streamFinished_ = false;
            streamError_ = false; streamErrorMsg_.clear();
            lastFinishReason_.clear();
            streamPromptTokens_ = streamCompletionTokens_ = 0;

            bool ok = client.streamBlocking(req, makeCallbacks());
            if (cancelRequested_.load()) break;

            if (!ok || streamError_) {
                std::string err = streamError_ ? streamErrorMsg_ : "stream failed";
                if (err.find("HTTP 4") != std::string::npos) {
                    // Retry up to 3 times with compaction between attempts.
                    // HTTP 400 is often caused by context overflow — compaction
                    // / repair can shrink the request enough to succeed.
                    static constexpr int MAX_HTTP_RETRIES = 3;
                    int httpRetry = 0;
                    while (httpRetry < MAX_HTTP_RETRIES) {
                        httpRetry++;
                        repairSession();
                        // Only compact if error is actually about context length
                        if (err.find("maximum context") != std::string::npos
                         || err.find("context_length") != std::string::npos
                         || err.find("too long") != std::string::npos
                         || err.find("token") != std::string::npos) {
                            doCompaction();
                        }
                        req = buildChatRequest();
                        currentContent_.clear(); currentReasoning_.clear();
                        currentToolCalls_.clear(); streamFinished_ = false;
                        streamError_ = false; streamErrorMsg_.clear();
                        streamPromptTokens_ = streamCompletionTokens_ = 0;
                        ok = client.streamBlocking(req, makeCallbacks());
                        if (cancelRequested_.load()) break;
                        if (ok && !streamError_) break;
                        err = streamError_ ? streamErrorMsg_ : "stream failed";
                        if (err.find("HTTP 4") == std::string::npos) break;
                    }
                    if (cancelRequested_.load()) break; // bail out cleanly on cancel
                    if (!ok || streamError_) {
                        session.popLastAssistant();
                        addPersistedMessage(Message::Assistant(
                            std::string(HTTP_RETRY_FAILED) + err));
                        phase_ = AgentPhase::Idle; break;
                    }
                } else {
                    phase_ = AgentPhase::Error;
                    { std::lock_guard<std::mutex> lk(snapshotMutex_); status_.errorMessage = err; }
                    break;
                }
            }
            if (cancelRequested_.load()) break;

            if (streamPromptTokens_ > 0 || streamCompletionTokens_ > 0) {
                if (onTokenUsage) onTokenUsage(streamPromptTokens_, streamCompletionTokens_);
            }

            if (!currentToolCalls_.empty()) {
                {
                    Message asst = Message::Assistant(currentContent_);
                    asst.toolCalls = currentToolCalls_;
                    asst.reasoningContent = currentReasoning_;
                    addPersistedMessage(asst);
                }
                pendingToolCalls_ = std::move(currentToolCalls_);
                toolIndex_ = 0; toolResults_.clear();

                bool hasD = false;
                for (auto& c : pendingToolCalls_) if (hasDestructiveCommand(c)) { hasD = true; break; }

                if (hasD && !destructiveApproved_.load()) {
                    pendingApprovalCalls_ = pendingToolCalls_;
                    phase_ = AgentPhase::AwaitApproval; updateSnapshot();
                    std::unique_lock<std::mutex> lk(approvalMutex_);
                    approvalCv_.wait(lk, [this]() { return approvalDone_ || cancelRequested_.load(); });
                    if (cancelRequested_.load()) break;
                    if (!approvalGranted_) {
                        session.clearLastToolCalls(USER_REJECTED_PREFIX);
                        phase_ = AgentPhase::Streaming; updateSnapshot(); break;
                    }
                }
                phase_ = AgentPhase::ExecutingTools; updateSnapshot();
            } else if (!currentContent_.empty()) {
                Message asst = Message::Assistant(currentContent_);
                asst.reasoningContent = currentReasoning_;
                addPersistedMessage(asst);
                phase_ = AgentPhase::Idle; updateSnapshot();
            } else if (!currentReasoning_.empty()) {
                // Stream finished with reasoning but no content.
                bool hitLength = (lastFinishReason_.find("length") != std::string::npos);
                if (hitLength && reasoningLengthRetries_ < 2 && !cancelRequested_.load()) {
                    // Reasoning consumed the whole output budget (max_tokens),
                    // so no final answer was generated. Retry with a bigger
                    // budget and an explicit instruction to stop over-thinking.
                    ++reasoningLengthRetries_;
                    configMaxTokens = std::min(configMaxTokens * 2, 65536);
                    addPersistedMessage(Message::System(
                        "[Auto-recovery] Reasoning consumed the entire output budget "
                        "(finish_reason=length) and no answer was produced. "
                        "Answer the user's request now with a concise final response. "
                        "Do NOT produce a long chain of thought. Keep thinking to a few "
                        "lines and output the final answer directly."));
                    phase_ = AgentPhase::Streaming; updateSnapshot();
                    break;
                }
                // Save reasoning so the user can see what happened,
                // and add a visible placeholder so the UI doesn't
                // silently jump to Idle.
                Message asst = Message::Assistant("[Stream ended after reasoning - no response generated]");
                asst.reasoningContent = currentReasoning_;
                addPersistedMessage(asst);
                phase_ = AgentPhase::Idle; updateSnapshot();
            } else {
                phase_ = AgentPhase::Idle; updateSnapshot();
            }
            break;
        }

        case AgentPhase::ExecutingTools: {
            updateSnapshot();
            bool wasCancelled = false;
            while (toolIndex_ < (int)pendingToolCalls_.size()) {
                if (cancelRequested_.load()) { wasCancelled = true; break; }
                auto& call = pendingToolCalls_[toolIndex_];
                {
                    std::lock_guard<std::mutex> lk(snapshotMutex_);
                    status_.toolProgressCurrent = toolIndex_ + 1;
                    status_.toolProgressTotal = (int)pendingToolCalls_.size();
                    status_.currentToolName = call.name;
                    status_.state = AgentState::ExecutingTool;
                }
                ToolResult r = executeTool(call);
                toolResults_.push_back(r);
                ++toolIndex_;
                if (r.isCancelled) { wasCancelled = true; break; }
            }
            if (cancelRequested_.load() || wasCancelled) {
                session.stripOrphanedToolCalls();
                break;
            }
            // Add results to session
            bool hasWV = false; std::string wvDetails;
            for (auto& r : toolResults_) {
                auto p = r.content.find("__WORKSPACE_VIOLATION__:");
                if (p != std::string::npos) {
                    hasWV = true;
                    std::string d = r.content.substr(p + 23);
                    auto s = d.find_first_not_of(" \t\n\r");
                    if (s != std::string::npos) d = d.substr(s);
                    wvDetails += "  - " + r.name + " -> " + d + "\n";
                }
                addPersistedMessage(Message::Tool(r.toolCallId, r.name, r.content, r.isError));
            }
            if (hasWV) {
                addPersistedMessage(Message::System(
                    std::string("[!] Workspace restriction: Outside workspace.\n\n") + wvDetails +
                    "\nCurrent workspace: " + (workspacePath_.empty() ? WS_VIOLATION_GUIDANCE_NO_WS : workspacePath_)));
            }
            // Error detection
            int consec = 0;
            for (auto it = toolResults_.rbegin(); it != toolResults_.rend(); ++it) {
                if (it->isError) ++consec; else break;
            }
            if (toolCallDepth_ >= maxToolCallsPerRound_) {
                toolCallDepth_ = 0;
                std::unordered_map<std::string,int> fcnt;
                for (auto& r : toolResults_) if (r.isError) fcnt[r.name]++;
                int mr = 0; std::string wt;
                for (auto& [n,c] : fcnt) if (c > mr) { mr = c; wt = n; }
                if (mr >= 2) {
                    session.softStripLastToolCalls();
                    addPersistedMessage(Message::System(
                        std::string(ERROR_STUCK_LOOP_PREFIX)+wt+ERROR_STUCK_LOOP_MIDDLE+
                        std::to_string(mr)+ERROR_STUCK_LOOP_SUFFIX));
                } else addPersistedMessage(Message::System(BATCH_COMPLETE));
            }
            if (consec >= 2) {
                toolCallDepth_ = 0;
                session.stripOrphanedToolCalls();
                addPersistedMessage(Message::System(ERROR_CONSECUTIVE_FAILURES));
                phase_ = AgentPhase::Streaming; pendingToolCalls_.clear(); toolResults_.clear();
                updateSnapshot(); break;
            }
            ++toolCallDepth_;
            doCompaction(); repairSession(); session.validateToolCallPairs();
            pendingToolCalls_.clear(); toolResults_.clear();
            phase_ = AgentPhase::Streaming; updateSnapshot();
            break;
        }

        case AgentPhase::AwaitApproval:
        case AgentPhase::Error:
            phase_ = AgentPhase::Idle; updateSnapshot(); break;
        }
    }
    phase_ = AgentPhase::Idle;
    {
        std::lock_guard<std::mutex> lk(snapshotMutex_);
        status_.state = AgentState::Idle; status_.statusMessage = "Ready";
        status_.streamingText.clear(); status_.reasoningText.clear();
        status_.toolProgressCurrent = status_.toolProgressTotal = 0; status_.currentToolName.clear();
    }
    debugLog("[Agent] run: END");
}

// ========== Public API ==========

void Agent::approveTool(int action) {
    { std::lock_guard<std::mutex> lk(approvalMutex_);
      approvalDone_ = true; approvalGranted_ = (action != 0);
      if (action == 2) destructiveApproved_.store(true, std::memory_order_release); }
    approvalCv_.notify_one();
}

void Agent::cancel() {
    cancelRequested_.store(true, std::memory_order_release);
    client.cancel();
    toolRegistry.cancelAll();
    { std::lock_guard<std::mutex> lk(approvalMutex_); approvalDone_ = true; approvalGranted_ = false; }
    approvalCv_.notify_one();
}

void Agent::newTurn() {
    cancelRequested_.store(false, std::memory_order_release);
    toolRegistry.resetCancel();
    approvalDone_ = false;
    currentToolCalls_.clear();
    pendingToolCalls_.clear();
    toolResults_.clear();
    currentContent_.clear();
    currentReasoning_.clear();
}

AgentStatus Agent::getStatus() const {
    std::lock_guard<std::mutex> lk(snapshotMutex_);
    return status_;
}

void Agent::clearSession() {
    cancel(); session.clear();
    contextTokensDirty_ = true;
    toolCallDepth_ = 0; destructiveApproved_.store(false);
    phase_ = AgentPhase::Idle;
    {
        std::lock_guard<std::mutex> lk(snapshotMutex_);
        status_.state = AgentState::Idle; status_.statusMessage = "Ready";
        status_.streamingText.clear(); status_.reasoningText.clear();
        status_.toolProgressCurrent = status_.toolProgressTotal = 0; status_.currentToolName.clear();
    }
}

void Agent::replaceSystemPrompt(const std::string& newPrompt) {
    systemPrompt_ = newPrompt;
    reloadAllowedTools();

    auto msgs = session.getContextMessages();
    for (auto& msg : msgs) {
        if (msg.role == "system") {
            msg.content = newPrompt;
            session.loadMessages(std::move(msgs));
            contextTokensDirty_ = true;
            return;
        }
    }
    // No existing system message -- add one (shouldn't normally happen)
    addPersistedMessage(Message::System(newPrompt));
}

void Agent::updateSnapshot() {
    std::lock_guard<std::mutex> lk(snapshotMutex_);
    switch (phase_.load()) {
        case AgentPhase::Idle:           status_.state = AgentState::Idle; break;
        case AgentPhase::Streaming:      status_.state = AgentState::Thinking; break;
        case AgentPhase::ExecutingTools: status_.state = AgentState::ExecutingTool; break;
        case AgentPhase::AwaitApproval:  status_.state = AgentState::AwaitingApproval; break;
        case AgentPhase::Error:          status_.state = AgentState::Error; break;
    }
}

// ========== Streaming callbacks ==========

StreamCallbacks Agent::makeCallbacks() {
    StreamCallbacks cb; Agent* self = this;
    auto batchCnt = std::make_shared<int>(0);
    static constexpr int BATCH_INTERVAL = 16; // update UI every ~16 tokens to reduce mutex contention
    cb.onText = [self, batchCnt](const std::string& t) {
        self->currentContent_ += t;
        if (++(*batchCnt) % BATCH_INTERVAL == 0) {
            std::lock_guard<std::mutex> lk(self->snapshotMutex_);
            self->status_.state = AgentState::Thinking;
            self->status_.streamingText = self->currentContent_;
            self->status_.reasoningText = self->currentReasoning_;
        }
    };
    cb.onThinking = [self, batchCnt](const std::string& t) {
        self->currentReasoning_ += t;
        if (++(*batchCnt) % BATCH_INTERVAL == 0) {
            std::lock_guard<std::mutex> lk(self->snapshotMutex_);
            self->status_.state = AgentState::Thinking;
            self->status_.streamingText = self->currentContent_;
            self->status_.reasoningText = self->currentReasoning_;
        }
    };
    cb.onToolCall = [self](const ToolCall& call) {
        if (!call.name.empty()) {
            ToolCall tc; tc.index = call.index;
            tc.id = call.id.empty() ? "call_"+std::to_string(call.index) : call.id;
            tc.name = call.name; tc.arguments = call.arguments;
            bool found = false;
            for (auto& e : self->currentToolCalls_) { if (e.index == call.index) { e = tc; found = true; break; } }
            if (!found) self->currentToolCalls_.push_back(tc);
        } else if (!call.arguments.empty()) {
            for (auto& e : self->currentToolCalls_) { if (e.index == call.index) { e.arguments += call.arguments; return; } }
            if (!self->currentToolCalls_.empty()) self->currentToolCalls_.back().arguments += call.arguments;
        }
    };
    cb.onFinishReason = [self](const std::string& reason) {
        self->lastFinishReason_ = reason;
    };
    cb.onFinish = [self]() {
        self->streamFinished_ = true;
        // Flush final state so UI sees the complete text
        std::lock_guard<std::mutex> lk(self->snapshotMutex_);
        self->status_.streamingText = self->currentContent_;
        self->status_.reasoningText = self->currentReasoning_;
    };
    cb.onUsage = [self](int p, int c) { self->streamPromptTokens_ = p; self->streamCompletionTokens_ = c; if (self->onTokenUsage) self->onTokenUsage(p,c); };
    cb.onError = [self](const std::string& e) { self->streamError_ = true; self->streamErrorMsg_ = e; };
    return cb;
}

// ========== Compaction ==========

void Agent::doCompaction() { if (!cancelRequested_.load()) if (!compactSession()) session.pruneForContext(effectiveContextWindow()); }

void Agent::checkContextWarning() {
    auto msgs = session.getContextMessages(); size_t total = 0;
    for (auto& m : msgs) {
        total += Session::estimateTokens(m.content) + Session::estimateTokens(m.reasoningContent);
        for (auto& tc : m.toolCalls) total += Session::estimateTokens(tc.name) + Session::estimateTokens(tc.arguments);
    }
    size_t w = effectiveContextWindow();
    if (total >= w * 3 / 4) { char b[256]; snprintf(b,sizeof(b),CONTEXT_WARNING_TEMPLATE,total/1000,w/1000); addPersistedMessage(Message::System(b)); }
}

bool Agent::compactSession() {
    if (cancelRequested_.load() || session.messageCount() < 8) return false;
    auto msgs = session.getContextMessages();
    size_t est = 0;
    for (auto& m : msgs) est += Session::estimateTokens(m.content) + Session::estimateTokens(m.reasoningContent);
    size_t w = effectiveContextWindow();
    if (est < w * 4 / 5) return false;
    auto plan = session.planCompaction(4);
    if (plan.summarizeIndices.empty()) return false;
    std::string input;
    { std::lock_guard<std::mutex> lk(todoMutex);
      std::string tc = buildTodoSystemMessage(todoData), ov = buildOverviewSystemMessage(todoData);
      if (!ov.empty()) input += ov + "\n\n"; if (!tc.empty()) input += tc + "\n\n"; }
    input += session.buildCompactionInput(plan);
    if (input.size() < 200) return false;
    ChatRequest req; req.model = model_;
    req.messages.push_back(Message::System(loadPromptFile("compactor.md")));
    req.messages.push_back(Message::User(input)); req.stream = false;
    req.maxTokens = configMaxTokens; req.temperature = 0.0;
    std::string err; ChatResponse resp = client.sendMessage(req, &err);
    if (!err.empty() || resp.messages.empty()) return false;
    std::string s = resp.messages.back().content;
    if (s.size() < 50) return false;
    session.applyCompaction(plan, s); return true;
}

// ========== Tool execution ==========

ToolResult Agent::executeTool(const ToolCall& call) {
    ToolResult r; r.toolCallId = call.id; r.name = call.name;
    if (!allowedTools_.empty() && std::find(allowedTools_.begin(), allowedTools_.end(), call.name) == allowedTools_.end()) {
        r.content = "Error: Tool '" + call.name + "' not allowed."; r.isError = true; return r;
    }
    r.content = toolRegistry.execute(call.name, call.arguments);
    r.isError = (r.content.compare(0,6,"Error:") == 0);
    if (toolRegistry.isCancelled()) r.isCancelled = true;
    return r;
}

bool Agent::hasDestructiveCommand(const ToolCall& call) const {
    if (call.name != "exec_shell") return false;
    std::string cmd;
    try { cmd = nlohmann::json::parse(call.arguments).value("command",""); } catch (...) { return false; }
    std::string lo = cmd; for (auto& c : lo) c = (char)tolower((unsigned char)c);
    struct K { const char* w; bool tb; };
    const K kw[] = {
        // "del" variants (CMD: del, PowerShell alias for Remove-Item)
        {"del ",1},{"del\t",1},{"del\"",1},{"del/",1},
        {"del;",1},{"del|",1},{"del>",1},{"del)",1},{"del&",1},
        // "erase" (CMD synonym for del)
        {"erase ",1},{"erase\t",1},
        // "rm" variants (Unix / PowerShell Remove-Item alias)
        {"rm ",1},{"rm\t",1},{"rm\"",1},{"rm/",1},
        {"rm;",1},{"rm|",1},{"rm>",1},{"rm)",1},{"rm&",1},
        // "rmdir" (CMD / PowerShell)
        {"rmdir ",1},{"rmdir\t",1},
        // "rd" variants (CMD remove directory)
        {"rd ",1},{"rd\t",1},{"rd\"",1},{"rd/",1},
        {"rd;",1},{"rd|",1},{"rd>",1},{"rd)",1},
        // "remove-item" variants (PowerShell)
        {"remove-item ",1},{"remove-item;",1},{"remove-item|",1},
        {"remove-item>",1},{"remove-item)",1},{"remove-item&",1},
        // "ri" (PowerShell shorthand for Remove-Item — requires token boundary)
        {"ri ",1},{"ri;",1},{"ri|",1},{"ri>",1},
        // destructive commands (no boundary check)
        {"format ",1},{"diskpart",0},{"taskkill",0},{"shutdown",0}
    };
    for (auto& k : kw) {
        size_t p = lo.find(k.w);
        while (p != std::string::npos) {
            bool ok = true;
            if (k.tb && p > 0) {
                char prev = lo[p-1];
                // Previous char must be a token boundary:
                // whitespace, quotes, shell operators (; | > ) &), or '='.
                ok = (prev==' '||prev=='\t'||prev=='\r'||prev=='\n'
                   ||prev=='"'||prev=='\''||prev==';'||prev=='|'
                   ||prev=='>'||prev==')'||prev=='&'||prev=='=');
            }
            if (ok) return true;
            p = lo.find(k.w, p+1);
        }
    }
    return false;
}

// ========== Build request ==========

ChatRequest Agent::buildChatRequest() const {
    ChatRequest req; req.messages = session.getContextMessages();
    req.stream = true; req.model = model_; req.maxTokens = configMaxTokens;
    req.temperature = configTemperature; req.tools = getFilteredToolDefinitions();
    if (!req.messages.empty()) {
        std::string tp = buildToolPathsMessage(cppCompilerPath, pythonPath);
        auto td = copyTodoData();
        std::string ur = buildUserRequestSystemMessage(td), tm = buildTodoSystemMessage(td), ov = buildOverviewSystemMessage(td);
        std::vector<Message> inj;
        if (!tp.empty()) inj.push_back(Message::System(tp));
        if (!ur.empty()) inj.push_back(Message::System(ur));
        if (!tm.empty()) inj.push_back(Message::System(tm));
        if (!ov.empty()) inj.push_back(Message::System(ov));
        if (!inj.empty()) req.messages.insert(req.messages.begin()+1, std::make_move_iterator(inj.begin()), std::make_move_iterator(inj.end()));
    }
    return req;
}

std::vector<ToolDefinition> Agent::getFilteredToolDefinitions() const {
    auto all = toolRegistry.getToolDefinitions();
    if (allowedTools_.empty()) return all;
    std::vector<ToolDefinition> f;
    for (auto& d : all) if (std::find(allowedTools_.begin(), allowedTools_.end(), d.name) != allowedTools_.end()) f.push_back(d);
    return f;
}

void Agent::repairSession() { session.repairOrphanedToolCalls(); }

// ========== Misc ==========

std::vector<Message> Agent::getNewMessagesSince(int64_t id) const {
    return (storage && storage->isOpen()) ? storage->queryMessages(id) : std::vector<Message>{};
}

void Agent::setModel(const std::string& m) { model_ = m; contextBudget = Session::computeContextBudget(effectiveContextWindow(), configMaxTokens); }
Session::PressureLevel Agent::getContextPressure() const { return contextBudget.getPressure(getEstimatedContextTokens()); }

size_t Agent::getEstimatedContextTokens() const {
    if (contextTokensDirty_) {
        size_t t = 0; auto msgs = session.getContextMessages();
        for (auto& m : msgs) {
            t += Session::estimateTokens(m.content) + Session::estimateTokens(m.reasoningContent);
            for (auto& tc : m.toolCalls) t += Session::estimateTokens(tc.name) + Session::estimateTokens(tc.arguments);
        }
        cachedContextTokens_ = t;
        contextTokensDirty_ = false;
    }
    return cachedContextTokens_;
}

void Agent::setToolPaths(const std::string& cpp, const std::string& py) { cppCompilerPath = cpp; pythonPath = py; }
void Agent::setRequestParams(int mt, double t) { configMaxTokens = mt; configTemperature = t; }

bool Agent::handleQuickCommand(const std::string& input) {
    std::string c = input;
    auto s = c.find_first_not_of(" \t\r\n"); if (s == std::string::npos) return false;
    auto e = c.find_last_not_of(" \t\r\n"); c = c.substr(s, e-s+1);
    if (c == "/clear") { clearSession(); addPersistedMessage(Message::Assistant(SESSION_CLEARED)); return true; }
    if (c == "/help") { addPersistedMessage(Message::Assistant(QUICK_HELP)); return true; }
    if (c == "/save") { saveRequested = true; addPersistedMessage(Message::Assistant(SAVE_TRIGGERED)); return true; }
    if (c == "/load") { loadRequested = true; addPersistedMessage(Message::Assistant(LOAD_TRIGGERED)); return true; }
    if (c == "/compress") {
        auto msgs = session.getContextMessages();
        size_t ot = 0; for (auto& m : msgs) ot += Session::estimateTokens(m.content);
        if (ot == 0) ot = 1;
        size_t ut=0,at=0,ta=0,tr=0;
        session.compressMessages(ut,at,ta,tr);
        msgs = session.getContextMessages(); size_t nt = 0;
        for (auto& m : msgs) nt += Session::estimateTokens(m.content);
        if (nt == 0) nt = 1;
        size_t saved = ot - nt;
        std::string r = "### Compression Report\n\n**Before:** ~"+std::to_string(ot)+" tok\n**After:** ~"+std::to_string(nt)+" tok\n**Saved:** ~"+std::to_string(saved)+" ("+std::to_string(saved*100/ot)+"%)\n\n**Details:**\n- User: "+std::to_string(ut)+"\n- Asst: "+std::to_string(at)+"\n- Args: "+std::to_string(ta)+"\n- Results: "+std::to_string(tr)+"\n";
        addPersistedMessage(Message::Assistant(r)); return true;
    }
    return false;
}