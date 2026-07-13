#include "agent.h"
#include "prompts.h"

// ============================================================================
// Templates & error strings (moved from prompts.h)
// ============================================================================

// Tool paths
static constexpr const char* TOOL_PATHS_HAVE =
    "[TOOL PATHS] Pre-configured tools -- use these:\n";
static constexpr const char* TOOL_PATHS_NONE =
    "[TOOL PATHS] No pre-configured tools. Search manually if needed.\n";

// Compaction prompt loaded from projv_prompts/compaction_prompt.md (or built-in default)
// via loadCompactionPrompt() in compactSession()

// Context warning
static constexpr const char* CONTEXT_WARNING_TEMPLATE =
    "Context is getting full (~%zuK/%zuK). "
    "Keep responses very concise, avoid redundant tool calls, "
    "and summarize instead of repeating file contents.";

// Error recovery
static constexpr const char* ERROR_CONSECUTIVE_FAILURES =
    "You have encountered multiple tool call errors in a row. "
    "Please provide your final answer now based on the information you have gathered. "
    "Do not use any more tools. Summarize what you found.";

static constexpr const char* ERROR_STUCK_LOOP_PREFIX =
    "\xe2\x9a\xa0\xef\xb8\x8f [Auto-recovery] You called '";
static constexpr const char* ERROR_STUCK_LOOP_MIDDLE =
    "' repeatedly and it failed ";
static constexpr const char* ERROR_STUCK_LOOP_SUFFIX =
    " times in a row.\n\nThe tool calls have been cleaned up, but the last result "
    "is preserved in the assistant message above for context.\n\n"
    "Please analyze the result and continue with a different approach, "
    "or summarize what you have done so far and explain where you're stuck.";

static constexpr const char* BATCH_COMPLETE =
    "[Batch complete. Continuing with remaining operations...]";

static constexpr const char* HTTP_RETRY_FAILED =
    "Error: API request failed after retry.\nError: ";

static constexpr const char* USER_REJECTED_PREFIX = "[User rejected the following tool call(s): ";

// Workspace violation guidance
static constexpr const char* WS_VIOLATION_GUIDANCE_PREFIX =
    "\xe2\x9a\xa0\xef\xb8\x8f [Workspace Restriction] The following file access attempts were rejected "
    "because they targeted paths outside the allowed workspace:\n\n";
static constexpr const char* WS_VIOLATION_GUIDANCE_MIDDLE =
    "\nCurrent workspace: ";
static constexpr const char* WS_VIOLATION_GUIDANCE_NO_WS =
    "(not set -- defaults to the directory where proJV.exe runs)";
static constexpr const char* WS_VIOLATION_GUIDANCE_SUFFIX =
    "\n\n"
    "YOUR RESPONSIBILITY -- you MUST tell the user what happened:\n"
    "1. Start your reply with a brief summary: \"I tried to read/edit [file] but it's outside the workspace.\"\n"
    "2. Show the user exactly which files you tried to access and why they were rejected.\n"
    "3. Give the user TWO clear options to fix this:\n"
    "   Option A: \"Expand the workspace -- edit config.toml, change workspace_path to include this directory.\"\n"
    "   Option B: \"Move/copy the needed file(s) into the current workspace.\"\n"
    "4. If you've already gathered useful information from accessible files, summarize that too.\n"
    "5. END WITH a question: \"Which option would you prefer?\" or \"Shall I try another approach?\"\n\n"
    "IMPORTANT: Do NOT retry accessing the same rejected paths. "
    "Once you've explained the situation and offered options, STOP and wait for the user's response.";

// UI strings (used by handleQuickCommand)
static constexpr const char* QUICK_HELP =
    "### Quick Commands\n\n"
    "| Command | Description |\n"
    "|---------|-------------|\n"
    "| `/clear` | Clear the current session |\n"
    "| `/help`  | Show this help message |\n"
    "| `/save`  | Save the current session to disk |\n"
    "| `/load`  | Load a previously saved session |\n"
    "| `/compress` | Summarize early messages to save context |\n";

static constexpr const char* SESSION_CLEARED = "Session cleared.";
static constexpr const char* SAVE_TRIGGERED = "Save triggered. Use the save dialog to choose a location.";
static constexpr const char* LOAD_TRIGGERED = "Load triggered. Use the load dialog to select a session file.";

// Convenience function: build tool paths message (moved from prompts.h)
static std::string buildToolPathsMessage(const std::string& cppPath, const std::string& pyPath) {
    if (!cppPath.empty() || !pyPath.empty()) {
        std::string msg = "[TOOL PATHS] Pre-configured tools -- use these:\n";
        if (!cppPath.empty()) msg += "- C++ compiler: " + cppPath + "\n";
        if (!pyPath.empty())  msg += "- Python: " + pyPath + "\n";
        return msg;
    }
    return "[TOOL PATHS] No pre-configured tools. Search manually if needed.\n";
}
#include "json.hpp"
#include "debug_log.h"
#include <cstdio>
#include <algorithm>
#include <iterator>
#include <sstream>
#include <set>
#include <unordered_map>

Agent::Agent(DeepSeekClient& client, ToolRegistry& tools)
    : client(client), toolRegistry(tools) {
    // Register the todo tool (needs agent state + mutex, so we do it here)
    registerTodoTool(tools, &todoData, &todoMutex);

    // Initialize context budget from effective window
    contextBudget = Session::computeContextBudget(effectiveContextWindow(), configMaxTokens);

    // Wire up the tool worker executor: delegate to Agent::executeTool
    toolWorkerMgr_.setExecutor([this](const ToolCall& call) -> ToolResult {
        return executeTool(call);
    });
}

// -- SQLite persistence -------------------------------------------

void Agent::addPersistedMessage(const Message& msg) {
    session.addMessage(msg);
    debugLogf("[Agent] addPersistedMessage: role=%s content=%zu chars", msg.role.c_str(), msg.content.size());
    if (storage && storage->isOpen()) {
        // Capture a copy of the message for the write queue
        Message copy = msg;
        auto doWrite = [storage = this->storage, copy = std::move(copy)]() {
            storage->insertMessage(copy);
        };
        if (enqueueWrite) {
            enqueueWrite(std::move(doWrite));
        } else {
            // Fallback: no queue configured, write directly (initialization phase)
            doWrite();
        }
    }
}

void Agent::loadFromStorage() {
    debugLog("[Agent] loadFromStorage: enter");
    if (!storage || !storage->isOpen()) {
        debugLog("[Agent] loadFromStorage: storage not open, returning");
        return;
    }
    debugLog("[Agent] loadFromStorage: calling queryMessages");
    auto msgs = storage->queryMessages(0);
    debugLogf("[Agent] loadFromStorage: queryMessages returned %zu msgs", msgs.size());
    debugLog("[Agent] loadFromStorage: calling session.loadMessages");
    session.loadMessages(std::move(msgs));
    debugLog("[Agent] loadFromStorage: done");
}

// -- Token estimation forward decl (defined below) -------------------
static size_t estimateTokens(const std::string& text);

// -- Context warning: inject when approaching limit ------------------
// Hard limit removed — use effectiveContextWindow() instead (model-aware).
// Warning fires at ~75% of effective window.

void Agent::checkContextWarning() {
    auto msgs = session.getContextMessages();
    debugLogf("[Agent] checkContextWarning: %zu messages", msgs.size());
    size_t total = 0;
    for (const auto& msg : msgs) {
        total += estimateTokens(msg.content);
        total += estimateTokens(msg.reasoningContent);
        for (const auto& tc : msg.toolCalls)
            total += estimateTokens(tc.name) + estimateTokens(tc.arguments);
    }
    size_t window = effectiveContextWindow();
    size_t warnAt = window * 3 / 4;  // 75%
    if (total >= warnAt) {
        debugLogf("[Agent] Context at ~%zu/%zu, inserting conciseness hint", total, window);
        char ctxWarnBuf[256];
        snprintf(ctxWarnBuf, sizeof(ctxWarnBuf), CONTEXT_WARNING_TEMPLATE, total / 1000, window / 1000);
        addPersistedMessage(Message::System(ctxWarnBuf));
    }
}

void Agent::sendMessage(const std::string& text) {
    debugLogf("[Agent] sendMessage: \"%s\"", truncateForLog(text, 200).c_str());

    // -- 懒初始化存储：用户发第一条消息时才创建 DB 文件 ----------
    if (ensureStorage) ensureStorage();

    // -- Save incomplete streaming content before cancel --
    {
        std::lock_guard<std::mutex> lock(statusMutex);
        if (!currentContent.empty() && status.state == AgentState::Thinking) {
            Message assistantMsg = Message::Assistant(currentContent);
            assistantMsg.toolCalls = pendingToolCalls;
            addPersistedMessage(assistantMsg);
            debugLogf("[Agent] Preserved incomplete streaming content (%zu chars) before cancel",
                currentContent.size());
        }
    }
    cancel();

    // Reset cancellation guard for the new operation.
    cancelled_.store(false, std::memory_order_release);
    operationGen_.fetch_add(1, std::memory_order_release);

    // Reset cancel flag for the new operation
    toolWorkerMgr_.requestCancel(); // ensure any stale worker is flagged

    // -- Save user prompt for todo tool (UI 线程内，早于后台任务) --------
    {
        std::lock_guard<std::mutex> lock(todoMutex);
        todoData.incomingUserPrompt = text;
    }

    addPersistedMessage(Message::User(text));
    setStatus(AgentState::Thinking, "Waiting for DeepSeek...");

    // -- R1/R5: 压缩 + 首次 API 请求移入后台线程，UI 立即返回 ----------
    // 捕获当前 operationGen_，回调落地前校验防串台。
    int gen = operationGen_.load(std::memory_order_acquire);

    std::thread([this, gen]() {
        // 校验代际：若已被新请求覆盖，放弃本次任务
        if (operationGen_.load(std::memory_order_acquire) != gen) {
            debugLog("[Agent] sendMessage bg: gen mismatch, aborting");
            return;
        }

        // Try LLM compaction first; fall back to mechanical pruning
        if (!compactSession()) {
            session.pruneForContext(effectiveContextWindow());
        }
        checkContextWarning();

        // 再次校验代际后再发 API
        if (operationGen_.load(std::memory_order_acquire) != gen) {
            debugLog("[Agent] sendMessage bg: gen mismatch after compaction, aborting");
            return;
        }

        sendToAPI();
    }).detach();
}

// -- Compaction prompt: ask the model to produce a structured summary ----------

bool Agent::compactSession() {
    // Don't compact if the user cancelled the operation
    if (cancelled_.load(std::memory_order_acquire)) {
        debugLog("[Agent] compactSession: cancelled, skipping");
        return false;
    }

    // Don't compact if we have fewer than 8 messages
    size_t msgCount = session.messageCount();
    if (msgCount < 8) return false;

    // Don't compact unless context is above threshold (~50% of window).
    // Compacting too early kills short-term memory and causes the model
    // to keep re-reading the same files (compaction deletes their contents).
    auto msgs = session.getContextMessages();
    size_t estimatedTokens = 0;
    for (const auto& m : msgs) {
        estimatedTokens += Session::estimateTokens(m.content);
        estimatedTokens += Session::estimateTokens(m.reasoningContent);
    }
    size_t window = effectiveContextWindow();
    size_t compactAt = window * 4 / 5; // only compact above 80%
    if (estimatedTokens < compactAt) {
        debugLogf("[Agent] compactSession: ~%zu/%zu tokens (below %zu threshold), skipping",
            estimatedTokens, window, compactAt);
        return false;
    }

    debugLogf("[Agent] compactSession: checking %zu messages (~%zu tokens)", msgCount, estimatedTokens);

    // Plan: which messages to keep vs summarize
    auto plan = session.planCompaction(4);
    if (plan.summarizeIndices.empty()) {
        debugLog("[Agent] compactSession: nothing to summarize");
        return false;
    }

    debugLogf("[Agent] compactSession: %zu pinned, %zu to summarize",
        plan.pinnedIndices.size(), plan.summarizeIndices.size());

    // Build input for the LLM — include TODO context for the compression model
    std::string input;

    // Prepend TODO overview + pending tasks as context for the compaction LLM
    {
        std::lock_guard<std::mutex> lock(todoMutex);
        std::string todoCtx = buildTodoSystemMessage(todoData);
        std::string overviewCtx = buildOverviewSystemMessage(todoData);
        if (!overviewCtx.empty()) input += overviewCtx + "\n\n";
        if (!todoCtx.empty()) input += todoCtx + "\n\n";
    }

    input += session.buildCompactionInput(plan);
    debugLogf("[Agent] compactSession: compaction input %zu chars", input.size());
    if (input.size() < 200) {
        debugLog("[Agent] compactSession: compaction input too small, skipping");
        return false;
    }

    // Send synchronous summarize request (use same model for context compatibility)
    ChatRequest req;
    req.model = model_;  // use same model to ensure context window compatibility
    req.messages.push_back(Message::System(loadCompactionPrompt()));
    req.messages.push_back(Message::User(input));
    req.stream = false;
    req.maxTokens = configMaxTokens;
    req.temperature = 0.0;

    std::string error;
    ChatResponse resp = client.sendMessage(req, &error);

    if (!error.empty()) {
        debugLogf("[Agent] compactSession: LLM call failed: %s", error.c_str());
        // Fall back to mechanical pruning
        return false;
    }

    if (resp.messages.empty()) {
        debugLog("[Agent] compactSession: LLM returned no messages");
        return false;
    }

    std::string summary = resp.messages.back().content;
    if (summary.size() < 50) {
        debugLog("[Agent] compactSession: summary too short, skipping");
        return false;
    }

    // Apply the summary
    session.applyCompaction(plan, summary);
    debugLogf("[Agent] compactSession: applied summary (%zu chars)", summary.size());
    return true;
}

void Agent::sendToAPI() {
    debugLog("[Agent] sendToAPI");
    // 清除上一轮的 tool 进度（本轮还没有工具要执行）
    {
        std::lock_guard<std::mutex> lock(statusMutex);
        status.toolProgressCurrent = 0;
        status.toolProgressTotal = 0;
        status.currentToolName.clear();
    }
    setStatus(AgentState::Thinking, "Waiting for DeepSeek...");
    // Reset cancellation guard for the new streaming request.
    cancelled_.store(false, std::memory_order_release);


    currentContent.clear();
    currentReasoning.clear();
    pendingToolCalls.clear();
    toolCallDepth.store(0, std::memory_order_release);

    // Repair session before sending to prevent "insufficient tool messages" error
    repairSession();
    session.validateToolCallPairs();

    ChatRequest req = buildChatRequest();
    // req.tools set by buildChatRequest (filtered by allowedTools_)

    if (!client.startStreaming(req, makeCallbacks())) {
        setError("Failed to start streaming");
    }
}

ChatRequest Agent::buildChatRequest() const {
    ChatRequest req;
    req.messages = session.getContextMessages();
    req.stream = true;
    req.model = model_;
    req.maxTokens = configMaxTokens;
    req.temperature = configTemperature;
    req.tools = getFilteredToolDefinitions();

    // -- Inject TOOL PATHS + USER REQUEST + TODO + Overview ------
    // Item 7: Batch insert all 4 injection messages at once (not 4 separate
    // insert calls), so messages at index 5+ keep stable positions for
    // prefix-cache. Each `insert` O(n)-shifts the tail; one batch insert
    // does a single shift.
    if (!req.messages.empty()) {
        std::string toolPathsMsg = buildToolPathsMessage(cppCompilerPath, pythonPath);
        auto todoCopy = copyTodoData();
        std::string userReqMsg = buildUserRequestSystemMessage(todoCopy);
        std::string todoMsg = buildTodoSystemMessage(todoCopy);
        std::string overviewMsg = buildOverviewSystemMessage(todoCopy);

        // Build all injection messages first, then batch-insert once
        std::vector<Message> injectMsgs;
        if (!toolPathsMsg.empty())
            injectMsgs.push_back(Message::System(toolPathsMsg));
        if (!userReqMsg.empty())
            injectMsgs.push_back(Message::System(userReqMsg));
        if (!todoMsg.empty())
            injectMsgs.push_back(Message::System(todoMsg));
        if (!overviewMsg.empty())
            injectMsgs.push_back(Message::System(overviewMsg));

        if (!injectMsgs.empty()) {
            req.messages.insert(
                req.messages.begin() + 1,
                std::make_move_iterator(injectMsgs.begin()),
                std::make_move_iterator(injectMsgs.end()));
        }
    }

    return req;
}

// -- Token estimation (rough: ~4 chars per token) ----------------------
static size_t estimateTokens(const std::string& text) {
    size_t t = 0;
    for (char c : text) {
        if ((unsigned char)c >= 0x80) t += 2;  // CJK / Unicode
        else t += 1;                            // ASCII
    }
    return std::max<size_t>(1, t / 4);
}

StreamCallbacks Agent::makeCallbacks() {
    StreamCallbacks cb;
    Agent* self = this;

    cb.onText = [self](const std::string& text) {
        std::lock_guard<std::mutex> lock(self->statusMutex);
        self->currentContent += text;
        self->status.streamingText = self->currentContent;
    };

    cb.onThinking = [self](const std::string& text) {
        std::lock_guard<std::mutex> lock(self->statusMutex);
        self->currentReasoning += text;
        self->status.reasoningText = self->currentReasoning;
    };

    cb.onToolCall = [self](const ToolCall& call) {
        std::lock_guard<std::mutex> lock(self->statusMutex);
        if (!call.name.empty()) {
            ToolCall tc;
            tc.index = call.index;
            tc.id = call.id.empty()
                ? "call_" + std::to_string(call.index)
                : call.id;
            tc.name = call.name;
            tc.arguments = call.arguments;

            bool found = false;
            for (auto& existing : self->pendingToolCalls) {
                if (existing.index == call.index) {
                    existing = tc;
                    found = true;
                    break;
                }
            }
            if (!found)
                self->pendingToolCalls.push_back(tc);
        } else if (!call.arguments.empty()) {
            bool found = false;
            for (auto& existing : self->pendingToolCalls) {
                if (existing.index == call.index) {
                    existing.arguments += call.arguments;
                    found = true;
                    break;
                }
            }
            if (!found && !self->pendingToolCalls.empty())
                self->pendingToolCalls.back().arguments += call.arguments;
        }
    };

    cb.onFinish = [self]() {
        auto tid = std::this_thread::get_id();
        debugLogf("[Agent] onFinish: content=%zu chars pendingToolCalls=%zu (thread=%08X)",
            self->currentContent.size(), self->pendingToolCalls.size(), *(unsigned int*)&tid);
        // If user cancelled, don't add messages or launch new work.
        if (self->cancelled_.load(std::memory_order_acquire)) {
            self->setStatus(AgentState::Idle, "Cancelled");
            return;
        }

        std::vector<ToolCall> calls;
        std::string content;
        std::string thinking;
        {
            std::lock_guard<std::mutex> lock(self->statusMutex);
            calls.swap(self->pendingToolCalls);
            content.swap(self->currentContent);
            thinking.swap(self->currentReasoning);
        }

        if (!self->compactSession()) {
            self->session.pruneForContext(self->effectiveContextWindow());
        }
        self->checkContextWarning();

        if (!calls.empty()) {
            Message assistantMsg = Message::Assistant(content);
            assistantMsg.toolCalls = calls;
            assistantMsg.reasoningContent = std::move(thinking);
            self->addPersistedMessage(assistantMsg);
            self->queueOrExecuteToolCalls(calls);
        } else if (!content.empty()) {
            // Text-only reply: save reasoning for display, API serialization
            // guards against sending it without tool_calls (see to_json).
            Message finalMsg = Message::Assistant(content);
            finalMsg.reasoningContent = std::move(thinking);
            self->addPersistedMessage(finalMsg);
            self->setStatus(AgentState::Idle, "Ready");
        } else {
            self->setStatus(AgentState::Idle, "Ready");
        }
    };

    cb.onUsage = [self](int prompt, int completion) {
        if (self->onTokenUsage) {
            self->onTokenUsage(prompt, completion);
        }
    };

    cb.onError = [self](const std::string& error) {
        self->setError(error);
    };

    return cb;
}

// -- Shared destructive check: approve or execute --------------------
void Agent::queueOrExecuteToolCalls(const std::vector<ToolCall>& calls) {
    debugLogf("[Agent] queueOrExecuteToolCalls: %zu tool(s)", calls.size());
    bool hasDestructive = false;
    for (const auto& call : calls) {
        if (call.name == "exec_shell") {
            std::string cmd;
            try { cmd = nlohmann::json::parse(call.arguments).value("command", ""); }
            catch (...) {}
            debugLogf("[Agent] Checking exec_shell cmd: %s",
                truncateForLog(cmd, 500).c_str());

            std::string clower = cmd;
            for (auto& c : clower) c = (char)tolower((unsigned char)c);

            // Destructive keywords that must appear as a standalone token
            // (preceded by whitespace or at the start of the string).
            // This prevents false positives like "Model " matching "del ".
            struct KillWord {
                const char* word;
                bool tokenBoundary; // require preceding whitespace / start-of-string
            };
            const KillWord killWords[] = {
                {"del ", true},  {"del\t", true}, {"del\"", true}, {"del/", true},
                {"erase ", true},  {"erase\t", true},
                {"rm ", true},  {"rm\t", true}, {"rm\"", true}, {"rm/", true},
                {"rmdir ", true},  {"rmdir\t", true},
                {"rd ", true},  {"rd\t", true}, {"rd\"", true}, {"rd/", true},
                {"format ", true},
                {"remove-item ", true},
                // Multi-char commands that are unlikely to appear as substrings
                // — keep non-token-boundary to catch all variants.
                {"diskpart", false},
                {"taskkill", false},
                {"shutdown", false},
            };
            for (const auto& kw : killWords) {
                size_t pos = clower.find(kw.word);
                while (pos != std::string::npos) {
                    bool ok = true;
                    if (kw.tokenBoundary && pos > 0) {
                        char prev = clower[pos - 1];
                        // Only match if preceded by whitespace
                        ok = (prev == ' ' || prev == '\t' || prev == '\r' || prev == '\n');
                    }
                    if (ok) {
                        hasDestructive = true;
                        debugLogf("[Agent] DESTRUCTIVE detected: keyword='%s' in cmd='%s'",
                            kw.word, truncateForLog(cmd, 200).c_str());
                        break;
                    }
                    pos = clower.find(kw.word, pos + 1);
                }
                if (hasDestructive) break;
            }
            if (hasDestructive) break;
        }
    }

    // 进度回调：由 toolWorker 线程每执行完一个工具时调用，更新状态供 UI 读取
    auto makeProgressCb = [this](int gen) -> ToolWorkerManager::ProgressCallback {
        return [this, gen](int current, int total, const std::string& toolName) {
            if (operationGen_.load(std::memory_order_acquire) != gen) return;
            std::lock_guard<std::mutex> lock(statusMutex);
            status.toolProgressCurrent = current;
            status.toolProgressTotal = total;
            status.currentToolName = toolName;
            status.statusMessage = toolName;
        };
    };

    if (hasDestructive) {
        // Session-level auto-approve: if user already approved once, skip dialog
        if (destructiveApproved.load(std::memory_order_acquire)) {
            debugLog("[Agent] Destructive auto-approved (session-level approval active)");
            int gen = operationGen_.load(std::memory_order_acquire);
            toolWorkerMgr_.launch(calls,
                [this, gen](const std::vector<ToolResult>& results) {
                    if (operationGen_.load(std::memory_order_acquire) != gen) return;
                    onToolResults(results);
                },
                makeProgressCb(gen));
            return;
        }
        std::lock_guard<std::mutex> lock(statusMutex);
        pendingApprovalCalls = calls;
        status.state = AgentState::AwaitingApproval;
        status.statusMessage = "Delete operation requires approval";
        debugLogf("[Agent] %zu destructive tool call(s) awaiting approval", calls.size());
    } else {
        debugLog("[Agent] No destructive tool detected, executing immediately");
        int gen = operationGen_.load(std::memory_order_acquire);
        toolWorkerMgr_.launch(calls,
            [this, gen](const std::vector<ToolResult>& results) {
                if (operationGen_.load(std::memory_order_acquire) != gen) return;
                onToolResults(results);
            },
            makeProgressCb(gen));
    }
}

// -- onToolResults: callback from ToolWorkerManager -------------------
// Called on the tool worker thread after all tools complete.
// Adds tool result messages to session, runs error detection,
// and continues to API.
void Agent::onToolResults(const std::vector<ToolResult>& results) {
    auto tid = std::this_thread::get_id();
    debugLogf("[Agent] onToolResults: %zu result(s) (thread=%08X)", results.size(), *(unsigned int*)&tid);
    // If user cancelled, don't add results or change status.
    if (cancelled_.load(std::memory_order_acquire)) {
        debugLog("[Agent] onToolResults: cancelled, returning");
        return;
    }

    bool batchLimitReached = false;
    int depth = toolCallDepth.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (depth >= maxToolCallsPerRound) {
        toolCallDepth.store(0, std::memory_order_release);
        batchLimitReached = true;
        debugLogf("[Agent] Tool call limit (%d) reached, forcing text summary", maxToolCallsPerRound);
    }

    // -- Detect workspace violations & collect details ----------------------
    // Instead of hard-stopping the agent with setError(), let the model
    // see the violation in the tool result and decide how to handle it.
    // Collect violation info so we can inject a precise, actionable message.
    bool hasWorkspaceViolation = false;
    std::string violationDetails;
    for (const auto& result : results) {
        auto pos = result.content.find("__WORKSPACE_VIOLATION__:");
        if (pos != std::string::npos) {
            hasWorkspaceViolation = true;
            // Extract the violation message (skip the prefix)
            std::string detail = result.content.substr(pos + 23); // strlen("__WORKSPACE_VIOLATION__:")
            // Trim leading whitespace
            size_t start = detail.find_first_not_of(" \t\n\r");
            if (start != std::string::npos) detail = detail.substr(start);
            violationDetails += "  - " + result.name + " → " + detail + "\n";
        }
    }

    // -- Add tool result messages to session ---------------------------
    std::string toolNames;
    for (size_t i = 0; i < results.size(); ++i) {
        if (i > 0) toolNames += ", ";
        toolNames += results[i].name;
        addPersistedMessage(Message::Tool(results[i].toolCallId, results[i].name,
                                          results[i].content, results[i].isError));
    }

    // -- Guide the model after workspace violation ------------------------
    if (hasWorkspaceViolation) {
        debugLog("[Agent] Workspace violation detected — injecting guidance");
        // Build workspace info
        std::string wsInfo = workspacePath_.empty()
            ? WS_VIOLATION_GUIDANCE_NO_WS
            : workspacePath_;
        addPersistedMessage(Message::System(
            std::string(WS_VIOLATION_GUIDANCE_PREFIX) +
            violationDetails +
            WS_VIOLATION_GUIDANCE_MIDDLE + wsInfo +
            WS_VIOLATION_GUIDANCE_SUFFIX));
    }

    setStatus(AgentState::ExecutingTool, toolNames);

    // -- Error loop detection (uses ToolWorkerManager cache) -----------
    int consecutiveErrors = toolWorkerMgr_.countConsecutiveToolErrors();
    if (consecutiveErrors >= 2) {
        toolCallDepth.store(0, std::memory_order_release);
        debugLogf("[Agent] Detected %d consecutive tool errors, forcing text summary", consecutiveErrors);

        // Strip orphaned tool_calls before sending
        session.stripOrphanedToolCalls();

        Message sysMsg = Message::System(
            ERROR_CONSECUTIVE_FAILURES);
        addPersistedMessage(sysMsg);

        // Use streaming for the continuation
        continueWithToolResults(results);
        return;
    }

    // -- Batch-limit reached: check for stuck loops --------------------
    if (batchLimitReached) {
        // Check for repeated identical failing tool calls using the manager's cache
        int maxRepeats = 0;
        std::string worstTool;
        for (const auto& result : results) {
            int count = toolWorkerMgr_.countRecentFailures(result.name);
            if (count > maxRepeats) {
                maxRepeats = count;
                worstTool = result.name;
            }
        }

        if (maxRepeats >= 2) {
            debugLogf("[Agent] Tool stuck loop detected: '%s' failed %d times, forcing summary",
                worstTool.c_str(), maxRepeats);

            // Use soft strip so tool result content is preserved in assistant message
            session.softStripLastToolCalls();
            addPersistedMessage(Message::System(
                std::string(ERROR_STUCK_LOOP_PREFIX) + worstTool +
                ERROR_STUCK_LOOP_MIDDLE + std::to_string(maxRepeats) +
                ERROR_STUCK_LOOP_SUFFIX));
        } else {
            addPersistedMessage(Message::System(BATCH_COMPLETE));
        }
    }

    // -- Continue to API -------------------------------------------------
    // Don't start new streaming if the user cancelled during tool execution.
    if (!cancelled_.load(std::memory_order_acquire)) {
        continueWithToolResults(results);
    }
    toolCallDepth.store(0, std::memory_order_release);
}

void Agent::continueWithToolResults(const std::vector<ToolResult>& results) {
    auto tid = std::this_thread::get_id();
    debugLogf("[Agent] continueWithToolResults (thread=%08X)", *(unsigned int*)&tid);
    // Try LLM compaction first; fall back to mechanical pruning
    if (!compactSession()) {
        session.pruneForContext(effectiveContextWindow());
    }
    // Pruning can orphan tool_results (by deleting their preceding assistant
    // with tool_calls).  Repair before sending to avoid HTTP 400.
    repairSession();
    session.validateToolCallPairs();

    // Check cancel flag before making the API call
    if (toolWorkerMgr_.isCancelled()) {
        setStatus(AgentState::Idle, "Cancelled");
        return;
    }

    // -- Use streaming (non-blocking) for the continue request ---------
    setStatus(AgentState::Thinking, "Continuing...");

    currentContent.clear();
    currentReasoning.clear();
    pendingToolCalls.clear();

    ChatRequest req;
    req.messages = session.getContextMessages();
    req.tools = getFilteredToolDefinitions();
    req.stream = true;

    if (!client.startStreaming(req, makeCallbacks())) {
        // -- Streaming failed; fall back to synchronous with retry ------
        // (only triggered on internal error, not normal HTTP flow)
        req.stream = false;
        std::string error;
        ChatResponse resp = client.sendMessage(req, &error);

        if (!error.empty()) {
            // Auto-retry on HTTP 4xx with session repair
            if (error.find("HTTP 4") != std::string::npos) {
                debugLog("[Agent] HTTP 4xx error, attempting session repair and retry...");
                repairSession();
                session.validateToolCallPairs();
                ChatRequest retryReq;
                retryReq.messages = session.getContextMessages();
                retryReq.tools = getFilteredToolDefinitions();
                retryReq.stream = false;
                std::string retryError;
                resp = client.sendMessage(retryReq, &retryError);
                if (!retryError.empty()) {
                    session.popLastAssistant();
                    Message errMsg = Message::Assistant(
                        std::string(HTTP_RETRY_FAILED) + retryError);
                    addPersistedMessage(errMsg);
                    setStatus(AgentState::Idle, "Ready");
                    return;
                }
                debugLog("[Agent] Session repair + retry succeeded");
            } else {
                setError(error);
                return;
            }
        }

        // Process synchronous fallback response
        if (onTokenUsage && (resp.promptTokens > 0 || resp.completionTokens > 0)) {
            onTokenUsage(resp.promptTokens, resp.completionTokens);
        }
        for (const auto& m : resp.messages) addPersistedMessage(m);
        for (const auto& m : resp.messages) {
            if (!m.toolCalls.empty()) { queueOrExecuteToolCalls(m.toolCalls); return; }
        }
        setStatus(AgentState::Idle, "Ready");
        if (!resp.messages.empty()) {
            std::lock_guard<std::mutex> lock(statusMutex);
            status.streamingText = resp.messages.back().content;
        }
    }
    // Streaming path: makeCallbacks handles the rest asynchronously
}

ToolResult Agent::executeTool(const ToolCall& call) {
    ToolResult result;
    result.toolCallId = call.id;
    result.name = call.name;

    // -- Whitelist check: only tools in allowedTools_ may execute.
    // If allowedTools_ is empty (no "tools available:" line in prompt),
    // all registered tools are allowed (backward compat).
    if (!allowedTools_.empty()) {
        auto it = std::find(allowedTools_.begin(), allowedTools_.end(), call.name);
        if (it == allowedTools_.end()) {
            result.content = "Error: Tool '" + call.name + "' is not in the allowed tools list.";
            result.isError = true;
            return result;
        }
    }

    result.content = toolRegistry.execute(call.name, call.arguments);
    // Actual tool errors start with "Error:" (e.g. "Error: Cannot open file").
    // Source code content may contain "Error:" deeper in the text, but that
    // does NOT indicate a tool failure — only a leading "Error:" does.
    result.isError = (result.content.compare(0, 6, "Error:") == 0);
    return result;
}

std::vector<ToolDefinition> Agent::getFilteredToolDefinitions() const {
    auto allDefs = toolRegistry.getToolDefinitions();
    if (allowedTools_.empty()) return allDefs;  // no filter = all allowed
    std::vector<ToolDefinition> filtered;
    for (const auto& def : allDefs) {
        if (std::find(allowedTools_.begin(), allowedTools_.end(), def.name) != allowedTools_.end())
            filtered.push_back(def);
    }
    return filtered;
}

void Agent::approveTool(int action) {
    // action: 0=reject, 1=approve once, 2=always allow this session
    {
        std::lock_guard<std::mutex> lock(statusMutex);
        debugLogf("[Agent] approveTool: action=%d currentState=%d pendingCalls=%zu",
            action, (int)status.state, pendingApprovalCalls.size());
    }
    std::vector<ToolCall> calls;
    {
        std::lock_guard<std::mutex> lock(statusMutex);
        if (status.state != AgentState::AwaitingApproval) return;
        calls = pendingApprovalCalls;
        pendingApprovalCalls.clear();
        status.state = AgentState::Idle;
        status.statusMessage = (action == 0) ? "Rejected" : "Executing...";
    }

    if (action == 0) {
        debugLog("[Agent] Tool call rejected by user");
        session.clearLastToolCalls("[User rejected the following tool call(s): ");
        setStatus(AgentState::Idle, "Ready");
        return;
    }

    if (action == 2) {
        debugLog("[Agent] Destructive auto-approved for this session");
        destructiveApproved.store(true, std::memory_order_release);
    }

    int gen = operationGen_.load(std::memory_order_acquire);
    toolWorkerMgr_.launch(calls,
        [this, gen](const std::vector<ToolResult>& results) {
            if (operationGen_.load(std::memory_order_acquire) != gen) return;
            onToolResults(results);
        },
        [this, gen](int current, int total, const std::string& toolName) {
            if (operationGen_.load(std::memory_order_acquire) != gen) return;
            std::lock_guard<std::mutex> lock(statusMutex);
            status.toolProgressCurrent = current;
            status.toolProgressTotal = total;
            status.currentToolName = toolName;
            status.statusMessage = toolName;
        });
}

void Agent::cancel() {
    {
        std::lock_guard<std::mutex> lock(statusMutex);
        debugLogf("[Agent] cancel: currentState=%d depth=%d busy=%d",
            (int)status.state, toolCallDepth.load(), toolWorkerMgr_.isBusy() ? 1 : 0);
    }
    // Set cancellation guard to prevent onFinish callbacks from re-launching.
    cancelled_.store(true, std::memory_order_release);
    operationGen_.fetch_add(1, std::memory_order_release);
    toolWorkerMgr_.cancelAndJoin();
    client.cancel();
    setStatus(AgentState::Idle, "Cancelled");
}

AgentStatus Agent::getStatus() const {
    std::lock_guard<std::mutex> lock(statusMutex);
    return status;
}

void Agent::clearSession() {
    debugLog("[Agent] clearSession");
    cancel();
    session.clear();
    toolCallDepth.store(0, std::memory_order_release);
    destructiveApproved.store(false, std::memory_order_release);
    setStatus(AgentState::Idle, "Ready");
}

void Agent::setModel(const std::string& model) {
    model_ = model;
    contextBudget = Session::computeContextBudget(effectiveContextWindow(), configMaxTokens);
    debugLogf("[Agent] Model set to: %s (context window: %zu)",
        model_.c_str(), effectiveContextWindow());
}

Session::PressureLevel Agent::getContextPressure() const {
    size_t total = getEstimatedContextTokens();
    return contextBudget.getPressure(total);
}

size_t Agent::getEstimatedContextTokens() const {
    size_t total = 0;
    auto msgs = session.getContextMessages();
    for (const auto& msg : msgs) {
        total += Session::estimateTokens(msg.content);
        total += Session::estimateTokens(msg.reasoningContent);
        for (const auto& tc : msg.toolCalls)
            total += Session::estimateTokens(tc.name) + Session::estimateTokens(tc.arguments);
    }
    return total;
}

void Agent::setToolPaths(const std::string& cpp, const std::string& python) {
    cppCompilerPath = cpp;
    pythonPath = python;
    debugLogf("[Agent] Tool paths updated: cpp=%s python=%s",
        cpp.empty() ? "(none)" : cpp.c_str(),
        python.empty() ? "(none)" : python.c_str());
}

void Agent::setRequestParams(int maxTokens, double temperature) {
    configMaxTokens = maxTokens;
    configTemperature = temperature;
    debugLogf("[Agent] Request params: maxTokens=%d temperature=%.2f", maxTokens, temperature);
}

bool Agent::handleQuickCommand(const std::string& input) {
    std::string cmd = input;
    size_t start = cmd.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return false;
    size_t end = cmd.find_last_not_of(" \t\r\n");

    cmd = cmd.substr(start, end - start + 1);

    if (cmd == "/clear") {
        clearSession();
        addPersistedMessage(Message::Assistant(SESSION_CLEARED));
        return true;
    }

    if (cmd == "/help") {
        std::string help = QUICK_HELP;
        addPersistedMessage(Message::Assistant(help));
        return true;
    }

    if (cmd == "/save") {
        saveRequested = true;
        addPersistedMessage(Message::Assistant(SAVE_TRIGGERED));
        return true;
    }

    if (cmd == "/load") {
        loadRequested = true;
        addPersistedMessage(Message::Assistant(LOAD_TRIGGERED));
        return true;
    }

    if (cmd == "/compress") {
        auto msgs = session.getContextMessages();
        size_t originalTokens = 0;
        for (const auto& m : msgs)
            originalTokens += Session::estimateTokens(m.content);
        if (originalTokens <= 0) originalTokens = 1;

        size_t userTrimmed = 0, assistantTrimmed = 0, toolArgsTrimmed = 0, toolResultsTrimmed = 0;

        session.compressMessages(userTrimmed, assistantTrimmed, toolArgsTrimmed, toolResultsTrimmed);

        msgs = session.getContextMessages();
        size_t newTokens = 0;
        for (const auto& m : msgs)
            newTokens += Session::estimateTokens(m.content);
        if (newTokens <= 0) newTokens = 1;
        size_t saved = originalTokens - newTokens;

        std::string report = "### Compression Report\n\n";
        report += "**Before:** ~" + std::to_string(originalTokens) + " tokens\n";
        report += "**After:** ~" + std::to_string(newTokens) + " tokens\n";
        report += "**Saved:** ~" + std::to_string(saved) + " tokens (" + std::to_string(saved * 100 / originalTokens) + "%)\n\n";
        report += "**Details:**\n";
        report += "- User messages trimmed: " + std::to_string(userTrimmed) + "\n";
        report += "- Assistant messages trimmed: " + std::to_string(assistantTrimmed) + "\n";
        report += "- Tool call arguments trimmed: " + std::to_string(toolArgsTrimmed) + "\n";
        report += "- Tool results trimmed: " + std::to_string(toolResultsTrimmed) + "\n";

        addPersistedMessage(Message::Assistant(report));
        return true;
    }

    return false;
}

// -- repairSession for HTTP 400 recovery ----------------------------
void Agent::repairSession() {
    debugLog("[Agent] repairSession");
    session.repairOrphanedToolCalls();
}

// -- Incremental message access for UI --------------------------------
std::vector<Message> Agent::getNewMessagesSince(int64_t sinceId) const {
    if (!storage || !storage->isOpen()) return {};
    auto msgs = storage->queryMessages(sinceId);
    if (!msgs.empty()) debugLogf("[Agent] getNewMessagesSince: sinceId=%lld returned %zu msgs", (long long)sinceId, msgs.size());
    return msgs;
}

void Agent::setStatus(AgentState state, const std::string& msg) {
    std::lock_guard<std::mutex> lock(statusMutex);
    // Guard: don't let Idle overwrite AwaitingApproval.
    // This prevents a spurious second onFinish() from the SSE flush
    // (after queueOrExecuteToolCalls set AwaitingApproval) from
    // resetting back to Idle, which would swallow the approval dialog.
    if (status.state == AgentState::AwaitingApproval && state == AgentState::Idle)
        return;
    status.state = state;
    status.statusMessage = msg;
    if (state == AgentState::Idle) {
        status.streamingText.clear();
        status.reasoningText.clear();
        status.toolProgressCurrent = 0;
        status.toolProgressTotal = 0;
        status.currentToolName.clear();
    }
}

void Agent::setError(const std::string& err) {
    std::lock_guard<std::mutex> lock(statusMutex);
    status.state = AgentState::Error;
    status.errorMessage = err;
    status.statusMessage = "Error: " + err;
}