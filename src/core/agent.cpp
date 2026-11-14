#include "agent.h"
#include "prompts.h"
#include "tools/wish_tool.h"
#include "json.hpp"
#include "debug_log.h"
#include "snapshot.h"
#include "tools/diff_capture.h"
#include <cstdio>
#include <algorithm>
#include <ctime>
#include <filesystem>
#include <iterator>
#include <sstream>
#include <set>
#include <unordered_map>

// ============================================================================
// Templates & error strings
// ============================================================================
static constexpr const char* CONTEXT_WARNING_TEMPLATE =
    "Context is getting full (~%zuK/%zuK). Keep responses very concise.";
static constexpr const char* ERROR_CONSECUTIVE_FAILURES =
    "Multiple tool call errors. Give your final answer now. Do not use more tools.";
static constexpr const char* ERROR_STUCK_LOOP_PREFIX = "\xe2\x9a\xa0 [Auto-recovery] You called '";
static constexpr const char* ERROR_STUCK_LOOP_MIDDLE = "' repeatedly and it failed ";
static constexpr const char* ERROR_STUCK_LOOP_SUFFIX =
    " times.\n\nTool calls cleaned up, last result preserved. Continue differently.";
static constexpr const char* HTTP_RETRY_FAILED = "Error: API request failed after retry.\nError: ";
static constexpr const char* IMAGE_400_HINT =
    "\nHint: the current model may not support image input. Switch to a Flash "
    "model (F10) and resend, or send the message without @image paths.";
static constexpr const char* USER_REJECTED_PREFIX = "[User rejected the following tool call(s): ";
static constexpr const char* WS_VIOLATION_GUIDANCE_NO_WS = "(not set -- defaults to exe dir)";
static constexpr const char* QUICK_HELP =
    "### Quick Commands\n\n| Command | Description |\n|---------|-------------|\n"
    "| `/clear` | Clear session |\n| `/help`  | Show this help |\n"
    "| `/compress` | Summarize early messages |\n"
    "| `/undo` | Undo the file changes of the last turn (`/undo!` drops later manual edits too) |\n"
    "| `/restore N` | Roll the workspace back to before turn N (`/restore N confirm`) |\n"
    "| `/<skill> <topic>` | Run an installed multi-agent skill (e.g. `/monica D:\\dir`) |\n"
    "| `/skill list` | List installed skills |\n"
    "| `/wish <what you want>` | Decide the open questions yourself and keep working until "
    "the TODO list is empty (`/wish cancel` stops it; `/wish` alone shows progress) |\n"
    "| `/end_task <id>\\|all` | Stop a background task started with `exec_shell(background:"
    "true)` |\n"
    "Type `/` in the input box to see this list and pick from it.\n"
    "\nproJV v" PROJV_VERSION " - native C++ DeepSeek AI agent (FTXUI).\n";
static constexpr const char* SESSION_CLEARED = "Session cleared.";
static constexpr const char* WISH_SYSTEM_PREFIX = "[WISH] ";
// ============================================================================
// Agent
// ============================================================================

Agent::Agent(DeepSeekClient& client, ToolRegistry& tools) : client(client), toolRegistry(tools) {
    registerTodoTool(tools, &todoData, &todoMutex);
    registerWishTools(tools, &todoData, &todoMutex, &wish_, &wishMutex_);
    contextBudget = Session::computeContextBudget(effectiveContextWindow(), configMaxTokens);
}

Agent::~Agent() {
    cancel();
}

void Agent::addPersistedMessage(const Message& msg) {
    session.addMessage(msg);
    contextTokensDirty_ = true;
    if (storage && storage->isOpen()) {
        Message copy = msg;
        auto write = [st = storage, c = std::move(copy)]() mutable { st->insertMessage(c); };
        if (enqueueWrite)
            enqueueWrite(std::move(write));
        else
            write();
    }
}

void Agent::loadFromStorage() {
    if (!storage || !storage->isOpen()) return;
    session.loadMessages(storage->queryMessages(0));
    contextTokensDirty_ = true;
}

// ========== Lifecycle: startTurn + run ==========

void Agent::startTurn(const std::string& text, std::vector<ImageAttachment> images) {
    debugLogf("[Agent] startTurn: \"%s\" (%zu image(s))", truncateForLog(text, 200).c_str(),
              images.size());
    if (phase_.load() != AgentPhase::Idle) {
        cancel();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (ensureStorage) ensureStorage();
    {
        std::lock_guard<std::mutex> lk(todoMutex);
        todoData.incomingUserPrompt = text;
    }
    cancelRequested_.store(false, std::memory_order_release);
    // Persist the user message synchronously (main thread) so the frontend can
    // display it immediately on the next redraw, instead of waiting for the
    // background agent thread's run() to reach it.
    Message msg = Message::User(text);
    msg.images = std::move(images);
    addPersistedMessage(msg);
    approvalDone_ = false;
}

// --- Phase bookkeeping -----------------------------------------------------
namespace {
// The first JSON string value in `json` -- for a shell tool that is the
// command, which is exactly what a host terminal should show next to
// "waiting for approval". Falls back to the raw head of the arguments.
std::string firstJsonValue(const std::string& json) {
    for (size_t i = 0; i + 1 < json.size(); ++i) {
        if (json[i] != ':' || json[i + 1] != '"') continue;
        std::string out;
        size_t j = i + 2;
        while (j < json.size() && json[j] != '"') {
            if (json[j] == '\\' && j + 1 < json.size()) {
                out += json[j + 1];
                j += 2;
                continue;
            }
            out += json[j++];
        }
        return out;
    }
    return json;
}

// Single line, control characters flattened, hard cap. Hosts show this in a
// tab or a notification body, never in a multi-line box.
std::string oneLine(const std::string& in, size_t maxLen = 120) {
    std::string out;
    out.reserve(in.size());
    bool prevSpace = false;
    for (char ch : in) {
        const unsigned char c = static_cast<unsigned char>(ch);
        const bool space = (c < 0x20 || c == 0x7f);
        if (space) {
            if (!prevSpace && !out.empty()) out += ' ';
            prevSpace = true;
            continue;
        }
        prevSpace = false;
        out += ch;
        if (out.size() >= maxLen) break;
    }
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}
// One message for both readers: the user sees what happened, and the model is
// told in plain words that the files it edited are gone. Without that sentence
// the next reply still talks about the reverted edits.
static std::string snapshotReportText(const char* cmd, const snapshot::Report& r) {
    if (r.needsForce) {
        return std::string("[Snapshot] Not executed: ") + std::to_string(r.skipped.size()) +
               " file(s) changed after that turn. Re-run " + cmd +
               (std::string(cmd) == "/undo" ? "!" : " <turn> confirm") +
               " to discard those changes as well.";
    }
    if (!r.error.empty()) return std::string("[Snapshot] Not executed: ") + r.error;

    std::string s = std::string("[Snapshot] Undid turn ") + std::to_string(r.turnSeq) + ": " +
                    std::to_string(r.restored.size()) + " restored, " +
                    std::to_string(r.removed.size()) + " removed";
    if (!r.skipped.empty()) s += ", " + std::to_string(r.skipped.size()) + " skipped";
    s += ".";
    if (!r.restored.empty() || !r.removed.empty()) {
        s += " Those edits no longer exist -- do not assume the files still contain them.\n";
        for (const std::string& p : r.restored)
            s += "  M " + p + "\n";
        for (const std::string& p : r.removed)
            s += "  D " + p + "\n";
    }
    return s;
}

} // namespace

std::string Agent::sessionId() const {
    if (storage) {
        const std::string id = storage->sessionId();
        if (!id.empty()) return id;
    }
    // No database yet (lazy creation): one stable id per process, so /undo
    // still works inside this run.
    static const std::string fallback = [] {
        const std::time_t t = std::time(nullptr);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char buf[40];
        std::strftime(buf, sizeof(buf), "session_%Y%m%d_%H%M%S", &tm);
        return std::string(buf);
    }();
    return fallback;
}

std::string Agent::approvalSummary() const {
    std::lock_guard<std::mutex> lk(approvalMutex_);
    if (!pendingApproval_) return "approval needed";
    const ToolCall& c = *pendingApproval_;
    std::string detail = oneLine(firstJsonValue(c.arguments), 80);
    std::string s = "approve " + c.name;
    if (!detail.empty()) s += ": " + detail;
    return oneLine(s, 120);
}

std::optional<ToolCall> Agent::pendingApproval() const {
    std::lock_guard<std::mutex> lk(approvalMutex_);
    return pendingApproval_;
}

void Agent::setPhase(AgentPhase p, const std::string& msgIn) {
    phase_.store(p);
    if (!phaseCallback_) return;
    std::string msg = msgIn;
    if (msg.empty()) {
        switch (p) {
        case AgentPhase::Streaming:
            msg = "thinking";
            break;
        case AgentPhase::ExecutingTools: {
            std::lock_guard<std::mutex> lk(snapshotMutex_);
            msg = status_.currentToolName.empty() ? "tool" : ("tool: " + status_.currentToolName);
            break;
        }
        case AgentPhase::AwaitApproval:
            msg = approvalSummary();
            break;
        case AgentPhase::Error: {
            std::lock_guard<std::mutex> lk(snapshotMutex_);
            std::string e = status_.errorMessage;
            const size_t nl = e.find('\n');
            if (nl != std::string::npos) e = e.substr(0, nl);
            msg = e.empty() ? "error" : oneLine(e);
            break;
        }
        case AgentPhase::Idle:
            msg = "turn finished";
            break;
        }
    }
    phaseCallback_(p, msg);
}

void Agent::run() {
    debugLog("[Agent] run: BEGIN");

    // Snapshot turn: the baseline for /undo. Non-fatal by design -- a
    // read-only store or an oversized workspace switches undo off, it never
    // blocks the turn.
    std::string snapErr;
    const int snapSeq = snapshot::beginTurn(sessionId(), snapErr);
    if (snapSeq < 0 && !snapErr.empty())
        debugLogInfof("[Agent] snapshots off: %s", snapErr.c_str());

    // One-shot nudge for this turn (e.g. "the skill finished, report it").
    // Persisted as a System message: invisible in the chat bubble list, present
    // in the context for every request of this turn (retries included).
    if (!turnNudge_.empty()) {
        addPersistedMessage(Message::System(turnNudge_));
        turnNudge_.clear();
    }

    if (compactRequested_.exchange(false)) {
        setPhase(AgentPhase::Streaming);
        updateSnapshot();
        size_t before = getEstimatedContextTokens();
        debugLogf("[Compact] request: before=%zu tokens, %zu msgs", before, session.messageCount());
        doCompaction(true);
        size_t after = getEstimatedContextTokens();
        debugLogf("[Compact] request: after=%zu tokens, %zu msgs", after, session.messageCount());
        addPersistedMessage(Message::Assistant("### Compressed\n\n"
                                               "**Before:** ~" +
                                               std::to_string(before) +
                                               " tok\n"
                                               "**After:** ~" +
                                               std::to_string(after) +
                                               " tok\n"
                                               "**Saved:** ~" +
                                               std::to_string(before > after ? before - after : 0) +
                                               " tok\n"));
        setPhase(AgentPhase::Idle);
    } else {
        doCompaction();
        checkContextWarning();
        reasoningLengthRetries_ = 0;
        setPhase(AgentPhase::Streaming);
        updateSnapshot();
    }

    while (phase_ != AgentPhase::Idle && !cancelRequested_.load()) {
        switch (phase_.load()) {

        case AgentPhase::Streaming: {
            updateSnapshot();
            ChatRequest req = buildChatRequest();
            currentContent_.clear();
            currentReasoning_.clear();
            currentToolCalls_.clear();
            streamFinished_ = false;
            streamError_ = false;
            streamErrorMsg_.clear();
            lastFinishReason_.clear();
            streamPromptTokens_ = streamCompletionTokens_ = 0;

            bool ok = client.streamBlocking(req, makeCallbacks());
            if (cancelRequested_.load()) break;

            if (!ok || streamError_) {
                std::string err = streamError_ ? streamErrorMsg_ : "stream failed";
                if (err.find("HTTP 4") != std::string::npos) {
                    // Retry up to 3 times with compaction between attempts.
                    // HTTP 400 is often caused by context overflow -- compaction
                    // / repair can shrink the request enough to succeed.
                    static constexpr int MAX_HTTP_RETRIES = 3;
                    int httpRetry = 0;
                    while (httpRetry < MAX_HTTP_RETRIES) {
                        httpRetry++;
                        repairSession();
                        // Only compact if error is actually about context length
                        if (err.find("maximum context") != std::string::npos ||
                            err.find("context_length") != std::string::npos ||
                            err.find("too long") != std::string::npos ||
                            err.find("token") != std::string::npos) {
                            doCompaction(true);
                        }
                        req = buildChatRequest();
                        currentContent_.clear();
                        currentReasoning_.clear();
                        currentToolCalls_.clear();
                        streamFinished_ = false;
                        streamError_ = false;
                        streamErrorMsg_.clear();
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
                        std::string failMsg = std::string(HTTP_RETRY_FAILED) + err;
                        // Vision: a 400 is the usual symptom of sending images to
                        // a model that does not accept them.
                        if (err.find("HTTP 400") != std::string::npos) {
                            bool hasImages = false;
                            for (const auto& m : session.getContextMessages())
                                if (!m.images.empty()) {
                                    hasImages = true;
                                    break;
                                }
                            if (hasImages) failMsg += IMAGE_400_HINT;
                        }
                        addPersistedMessage(Message::Assistant(failMsg));
                        setPhase(AgentPhase::Idle);
                        break;
                    }
                } else {
                    // Error text first: the phase callback reads status_.errorMessage
                    // to build the one-line report for the host terminal.
                    {
                        std::lock_guard<std::mutex> lk(snapshotMutex_);
                        status_.errorMessage = err;
                    }
                    setPhase(AgentPhase::Error);
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
                toolIndex_ = 0;
                toolResults_.clear();

                setPhase(AgentPhase::ExecutingTools);
                updateSnapshot();
            } else if (!currentContent_.empty()) {
                Message asst = Message::Assistant(currentContent_);
                asst.reasoningContent = currentReasoning_;
                addPersistedMessage(asst);
                setPhase(AgentPhase::Idle);
                updateSnapshot();
            } else if (!currentReasoning_.empty()) {
                // Stream finished with reasoning but no content.
                bool hitLength = (lastFinishReason_.find("length") != std::string::npos);
                if (hitLength && reasoningLengthRetries_ < 2 && !cancelRequested_.load()) {
                    // Reasoning consumed the whole output budget (max_tokens),
                    // so no final answer was generated. Retry with a bigger
                    // budget and an explicit instruction to stop over-thinking.
                    ++reasoningLengthRetries_;
                    // Never shrink the budget on a retry: the old
                    // min(x*2, 65536) left the 65536 default unchanged and cut a
                    // larger configured value in half.
                    const int doubled = (configMaxTokens > kMaxOutputTokens / 2)
                                            ? kMaxOutputTokens
                                            : configMaxTokens * 2;
                    configMaxTokens = std::max(configMaxTokens, doubled);
                    addPersistedMessage(Message::System(
                        "[Auto-recovery] Reasoning consumed the entire output budget "
                        "(finish_reason=length) and no answer was produced. "
                        "Answer the user's request now with a concise final response. "
                        "Do NOT produce a long chain of thought. Keep thinking to a few "
                        "lines and output the final answer directly."));
                    setPhase(AgentPhase::Streaming);
                    updateSnapshot();
                    break;
                }
                // Save reasoning so the user can see what happened,
                // and add a visible placeholder so the UI doesn't
                // silently jump to Idle.
                Message asst =
                    Message::Assistant("[Stream ended after reasoning - no response generated]");
                asst.reasoningContent = currentReasoning_;
                addPersistedMessage(asst);
                setPhase(AgentPhase::Idle);
                updateSnapshot();
            } else {
                setPhase(AgentPhase::Idle);
                updateSnapshot();
            }
            break;
        }

        case AgentPhase::ExecutingTools: {
            updateSnapshot();
            bool wasCancelled = false;
            bool wasRejected = false;
            while (toolIndex_ < (int)pendingToolCalls_.size()) {
                if (cancelRequested_.load()) {
                    wasCancelled = true;
                    break;
                }
                auto& call = pendingToolCalls_[toolIndex_];
                {
                    std::lock_guard<std::mutex> lk(snapshotMutex_);
                    status_.toolProgressCurrent = toolIndex_ + 1;
                    status_.toolProgressTotal = (int)pendingToolCalls_.size();
                    status_.currentToolName = call.name;
                    status_.state = AgentState::ExecutingTool;
                }
                // Policy runs per call, not per batch: "allow this command" is
                // granted for one exact command text, so each call has to be
                // looked at on its own. A red line is refused without asking.
                const std::string command = commandOf(call);
                const delete_guard::Verdict verdict = call.name == "exec_shell"
                                                          ? evaluateCommandPolicy(command)
                                                          : delete_guard::Verdict{};
                if (verdict.decision == delete_guard::Decision::Deny) {
                    debugLogWarnf("[Agent] policy denied %s: %s", call.name.c_str(),
                                  verdict.reason.c_str());
                    ToolResult blocked;
                    blocked.toolCallId = call.id;
                    blocked.name = call.name;
                    blocked.content = "Error: blocked by policy: " + verdict.reason;
                    blocked.isError = true;
                    toolResults_.push_back(blocked);
                    ++toolIndex_;
                    continue;
                }
                if (policyNeedsApproval(verdict, command)) {
                    const bool granted = awaitApproval(call);
                    if (cancelRequested_.load()) {
                        wasCancelled = true;
                        break;
                    }
                    if (!granted) {
                        // Same semantics as before: the whole batch is dropped
                        // and the model is told, then the turn continues.
                        toolResults_.clear();
                        pendingToolCalls_.clear();
                        session.clearLastToolCalls(USER_REJECTED_PREFIX);
                        setPhase(AgentPhase::Streaming);
                        updateSnapshot();
                        wasRejected = true;
                        break;
                    }
                }
                // Per-tool report ("tool: read_file") -- the host can show which
                // tool is running instead of a generic "working".
                setPhase(AgentPhase::ExecutingTools, "tool: " + call.name);
                ToolResult r = executeTool(call);
                toolResults_.push_back(r);
                ++toolIndex_;
                if (r.isCancelled) {
                    wasCancelled = true;
                    break;
                }
            }
            if (wasRejected) break;
            if (cancelRequested_.load() || wasCancelled) {
                session.stripOrphanedToolCalls();
                break;
            }
            // Add results to session
            bool hasWV = false;
            std::string wvDetails;
            for (auto& r : toolResults_) {
                auto p = r.content.find("__WORKSPACE_VIOLATION__:");
                if (p != std::string::npos) {
                    hasWV = true;
                    std::string d = r.content.substr(p + 23);
                    auto s = d.find_first_not_of(" \t\n\r");
                    if (s != std::string::npos) d = d.substr(s);
                    wvDetails += "  - " + r.name + " -> " + d + "\n";
                }
                // The 4-argument factory does not know about the display-only
                // diff, so it is attached here; dropping it would break the
                // bubble silently and no tool-level test would notice.
                Message toolMsg = Message::Tool(r.toolCallId, r.name, r.content, r.isError);
                toolMsg.diff = r.diff;
                addPersistedMessage(toolMsg);
            }
            if (hasWV) {
                addPersistedMessage(Message::System(
                    std::string("[!] Workspace restriction: Outside workspace.\n\n") + wvDetails +
                    "\nCurrent workspace: " +
                    (workspacePath_.empty() ? WS_VIOLATION_GUIDANCE_NO_WS : workspacePath_)));
            }

            // A tool may ask to end the turn here (run_skill). Tool results are
            // already written, so the assistant(tool_calls) -> tool pairing is
            // intact; stopping before the next request keeps the model out of
            // the skill phase entirely.
            if (toolRegistry.consumeTurnEndRequest()) {
                debugLog("[Agent] turn end requested by a tool -- stopping after this round");
                pendingToolCalls_.clear();
                toolResults_.clear();
                setPhase(AgentPhase::Idle);
                updateSnapshot();
                break;
            }
            // Error detection
            int consec = 0;
            for (auto it = toolResults_.rbegin(); it != toolResults_.rend(); ++it) {
                if (it->isError)
                    ++consec;
                else
                    break;
            }
            // Stuck-loop detection: two failures in a row are already the signal, so
            // there is no counter to wait for. The message names the tool that keeps
            // failing, which is the actionable part.
            if (consec >= 2) {
                std::unordered_map<std::string, int> fcnt;
                for (auto& r : toolResults_)
                    if (r.isError) fcnt[r.name]++;
                int mr = 0;
                std::string wt;
                for (auto& [n, c] : fcnt)
                    if (c > mr) {
                        mr = c;
                        wt = n;
                    }
                session.stripOrphanedToolCalls();
                if (mr >= 2)
                    addPersistedMessage(Message::System(
                        std::string(ERROR_STUCK_LOOP_PREFIX) + wt + ERROR_STUCK_LOOP_MIDDLE +
                        std::to_string(mr) + ERROR_STUCK_LOOP_SUFFIX));
                else
                    addPersistedMessage(Message::System(ERROR_CONSECUTIVE_FAILURES));
                setPhase(AgentPhase::Streaming);
                pendingToolCalls_.clear();
                toolResults_.clear();
                updateSnapshot();
                break;
            }
            doCompaction();
            repairSession();
            session.validateToolCallPairs();
            pendingToolCalls_.clear();
            toolResults_.clear();
            setPhase(AgentPhase::Streaming);
            updateSnapshot();
            break;
        }

        case AgentPhase::AwaitApproval:
        case AgentPhase::Error:
            setPhase(AgentPhase::Idle);
            updateSnapshot();
            break;
        }
    }
    setPhase(AgentPhase::Idle);
    {
        std::lock_guard<std::mutex> lk(snapshotMutex_);
        status_.state = AgentState::Idle;
        status_.statusMessage = "Ready";
        status_.streamingText.clear();
        status_.reasoningText.clear();
        status_.toolProgressCurrent = status_.toolProgressTotal = 0;
        status_.currentToolName.clear();
    }
    if (snapSeq >= 0) {
        std::string endErr;
        if (!snapshot::endTurn(snapSeq, endErr))
            debugLogWarnf("[Agent] snapshot end failed: %s", endErr.c_str());
    }
    debugLog("[Agent] run: END");
}

// ========== Public API ==========

void Agent::approveTool(int action) {
    std::string toPersist;
    {
        std::lock_guard<std::mutex> lk(approvalMutex_);
        // "Always" binds to the exact command text of the call on screen, and is
        // refused in Strict mode: that mode exists so nothing can be remembered.
        if (action == 2 && pendingApproval_ && approvalMode() != ApprovalMode::Strict) {
            const std::string cmd = commandOf(*pendingApproval_);
            if (!cmd.empty() && std::find(grants_.begin(), grants_.end(), cmd) == grants_.end()) {
                grants_.push_back(cmd);
                toPersist = cmd;
            }
        }
        approvalAction_ = action;
        approvalDone_ = true;
        approvalGranted_ = (action != 0);
    }
    // Outside the lock: the callback writes config.toml, which must never run while
    // the agent thread is waiting on approvalMutex_.
    if (!toPersist.empty() && onGrantPersist) onGrantPersist(toPersist);
    approvalCv_.notify_one();
}

void Agent::cancel() {
    cancelRequested_.store(true, std::memory_order_release);
    client.cancel();
    toolRegistry.cancelAll();
    {
        std::lock_guard<std::mutex> lk(approvalMutex_);
        approvalDone_ = true;
        approvalGranted_ = false;
    }
    approvalCv_.notify_one();
}

void Agent::newTurn() {
    cancelRequested_.store(false, std::memory_order_release);
    toolRegistry.resetCancel();
    // Defensive: a turn-end request raised by a tool must never leak into the
    // next turn (e.g. the turn was cancelled before it could be honoured).
    (void)toolRegistry.consumeTurnEndRequest();
    {
        std::lock_guard<std::mutex> lk(approvalMutex_);
        approvalDone_ = false;
        approvalGranted_ = false;
    }
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
    cancel();
    session.clear();
    contextTokensDirty_ = true;
    {
        std::lock_guard<std::mutex> lk(wishMutex_);
        // /clear ends the wish but keeps nothing: the TODO is reset below with the
        // rest of the session state.
        wish_.clear();
    }
    {
        std::lock_guard<std::mutex> lk(approvalMutex_);
        // Grants are policy, not session state: they live in config.toml now, so
        // /clear must not silently revoke what the user allowed forever.
        pendingApproval_.reset();
    }
    setPhase(AgentPhase::Idle);
    {
        // R2: new session must not carry over overview/TODO/user-request context.
        // Previously only the message session was cleared; todoData survived and
        // kept injecting [OVERVIEW]/[TODO]/[USER REQUEST] system messages via
        // buildChatRequest(). Reset it here so new chat / open dialog / /clear
        // all start with a clean todo state.
        std::lock_guard<std::mutex> lk(todoMutex);
        todoData = TodoData{};
    }
    {
        std::lock_guard<std::mutex> lk(snapshotMutex_);
        status_.state = AgentState::Idle;
        status_.statusMessage = "Ready";
        status_.streamingText.clear();
        status_.reasoningText.clear();
        status_.toolProgressCurrent = status_.toolProgressTotal = 0;
        status_.currentToolName.clear();
    }
}

bool Agent::replaceEnvironmentBlock(const std::string& text) {
    const std::string marker = "[ENVIRONMENT]";
    auto msgs = session.getContextMessages();
    for (auto& msg : msgs) {
        if (msg.role != "system") continue;
        if (msg.content.compare(0, marker.size(), marker) != 0) continue;
        if (msg.content == text) return true; // nothing moved: keep the bytes
        msg.content = text;
        session.loadMessages(std::move(msgs));
        contextTokensDirty_ = true;
        return true;
    }
    return false;
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
    case AgentPhase::Idle:
        status_.state = AgentState::Idle;
        break;
    case AgentPhase::Streaming:
        status_.state = AgentState::Thinking;
        break;
    case AgentPhase::ExecutingTools:
        status_.state = AgentState::ExecutingTool;
        break;
    case AgentPhase::AwaitApproval:
        status_.state = AgentState::AwaitingApproval;
        break;
    case AgentPhase::Error:
        status_.state = AgentState::Error;
        break;
    }
}

// ========== Streaming callbacks ==========

StreamCallbacks Agent::makeCallbacks() {
    StreamCallbacks cb;
    Agent* self = this;
    auto batchCnt = std::make_shared<int>(0);
    // R1: flush every token so the TUI streams reasoning/content in real time.
    // (Legacy batched by 16 to reduce mutex contention; the UI reads the
    // snapshot at most once per frame, far slower than token arrival.)
    static constexpr int BATCH_INTERVAL = 1;
    cb.onText = [self, batchCnt](const std::string& t) {
        self->currentContent_ += t;
        if (++(*batchCnt) % BATCH_INTERVAL == 0) {
            {
                std::lock_guard<std::mutex> lk(self->snapshotMutex_);
                self->status_.state = AgentState::Thinking;
                self->status_.streamingText = self->currentContent_;
                self->status_.reasoningText = self->currentReasoning_;
            }
            if (self->onStreamingTick) self->onStreamingTick();
        }
    };
    cb.onThinking = [self, batchCnt](const std::string& t) {
        self->currentReasoning_ += t;
        if (++(*batchCnt) % BATCH_INTERVAL == 0) {
            {
                std::lock_guard<std::mutex> lk(self->snapshotMutex_);
                self->status_.state = AgentState::Thinking;
                self->status_.streamingText = self->currentContent_;
                self->status_.reasoningText = self->currentReasoning_;
            }
            if (self->onStreamingTick) self->onStreamingTick();
        }
    };
    cb.onToolCall = [self](const ToolCall& call) {
        if (!call.name.empty()) {
            ToolCall tc;
            tc.index = call.index;
            tc.id = call.id.empty() ? "call_" + std::to_string(call.index) : call.id;
            tc.name = call.name;
            tc.arguments = call.arguments;
            bool found = false;
            for (auto& e : self->currentToolCalls_) {
                if (e.index == call.index) {
                    e = tc;
                    found = true;
                    break;
                }
            }
            if (!found) self->currentToolCalls_.push_back(tc);
        } else if (!call.arguments.empty()) {
            for (auto& e : self->currentToolCalls_) {
                if (e.index == call.index) {
                    e.arguments += call.arguments;
                    return;
                }
            }
            if (!self->currentToolCalls_.empty())
                self->currentToolCalls_.back().arguments += call.arguments;
        }
    };
    cb.onFinishReason = [self](const std::string& reason) { self->lastFinishReason_ = reason; };
    cb.onFinish = [self]() {
        self->streamFinished_ = true;
        // Flush final state so UI sees the complete text
        std::lock_guard<std::mutex> lk(self->snapshotMutex_);
        self->status_.streamingText = self->currentContent_;
        self->status_.reasoningText = self->currentReasoning_;
    };
    cb.onUsage = [self](int p, int c) {
        self->streamPromptTokens_ = p;
        self->streamCompletionTokens_ = c;
        if (self->onTokenUsage) self->onTokenUsage(p, c);
    };
    cb.onError = [self](const std::string& e) {
        self->streamError_ = true;
        self->streamErrorMsg_ = e;
    };
    return cb;
}

// ========== Compaction ==========

void Agent::doCompaction(bool force) {
    if (cancelRequested_.load()) return;
    contextTokensDirty_ = true;
    compactSession(force);
    session.pruneForContext(contextBudget.availableInput);
    repairSession();
    contextTokensDirty_ = true;
}

void Agent::checkContextWarning() {
    auto msgs = session.getContextMessages();
    size_t total = 0;
    for (auto& m : msgs) {
        total += Session::estimateTokens(m.content) + Session::estimateTokens(m.reasoningContent);
        for (auto& tc : m.toolCalls)
            total += Session::estimateTokens(tc.name) + Session::estimateTokens(tc.arguments);
    }
    size_t w = effectiveContextWindow();
    if (total >= w * 3 / 4) {
        char b[256];
        snprintf(b, sizeof(b), CONTEXT_WARNING_TEMPLATE, total / 1000, w / 1000);
        addPersistedMessage(Message::System(b));
    }
}

bool Agent::compactSession(bool force) {
    if (cancelRequested_.load()) {
        debugLogWarnf("[Compact] skipped: cancelled");
        return false;
    }
    if (!force && session.messageCount() < 8) {
        debugLogWarnf("[Compact] skipped: <8 messages (%zu)", session.messageCount());
        return false;
    }
    auto msgs = session.getContextMessages();
    size_t est = 0;
    for (auto& m : msgs)
        est += Session::estimateTokens(m.content) + Session::estimateTokens(m.reasoningContent);
    size_t w = effectiveContextWindow();
    if (!force && est < w * 4 / 5) {
        debugLogWarnf("[Compact] skipped: below threshold (%zu < %zu)", est, w * 4 / 5);
        return false;
    }
    auto plan = session.planCompaction(4);
    if (plan.summarizeIndices.empty()) {
        debugLogWarnf("[Compact] skipped: plan has 0 messages to summarize (all pinned)");
        return false;
    }
    std::string input;
    {
        std::lock_guard<std::mutex> lk(todoMutex);
        std::string tc = buildTodoSystemMessage(todoData),
                    ov = buildOverviewSystemMessage(todoData);
        if (!ov.empty()) input += ov + "\n\n";
        if (!tc.empty()) input += tc + "\n\n";
    }
    input += session.buildCompactionInput(plan);
    if (input.size() < 200) {
        debugLogWarnf("[Compact] skipped: compaction input too small (%zu bytes)", input.size());
        return false;
    }
    ChatRequest req;
    req.model = model_;
    req.messages.push_back(Message::System(loadPromptFile("compactor.md")));
    req.messages.push_back(Message::User(input));
    req.stream = false;
    req.maxTokens = configMaxTokens;
    req.temperature = 0.0;
    std::string err;
    ChatResponse resp = client.sendMessage(req, &err);
    if (!err.empty() || resp.messages.empty()) {
        debugLogWarnf("[Compact] LLM call failed: %s",
                      err.empty() ? "empty response" : err.c_str());
        return false;
    }
    std::string s = resp.messages.back().content;
    if (s.size() < 50) {
        debugLogWarnf("[Compact] summary too short (%zu bytes)", s.size());
        return false;
    }
    session.applyCompaction(plan, s);
    debugLogf("[Compact] applied: summarized %zu messages -> summary %zu bytes",
              plan.summarizeIndices.size(), s.size());
    return true;
}

std::string Agent::summarizeConversationForSharing() {
    // 1. Local digest: the three per-turn blocks are already summaries the model
    //    wrote, so they are always worth sending even when the transcript is huge.
    std::string local;
    {
        std::lock_guard<std::mutex> lk(todoMutex);
        const std::string ur = buildUserRequestSystemMessage(todoData);
        const std::string ov = buildOverviewSystemMessage(todoData);
        const std::string tc = buildTodoSystemMessage(todoData);
        if (!ur.empty()) local += ur + "\n";
        if (!ov.empty()) local += ov + "\n";
        if (!tc.empty()) local += tc + "\n";
    }

    // 2. A bounded slice of the conversation, newest first. 12 KB is roughly 4k
    //    tokens: enough for the compactor to see what the work was, small enough that
    //    the summary request itself can never become the problem it solves.
    constexpr size_t kSliceBudget = 12 * 1024;
    constexpr size_t kPerMessageCap = 1500;
    auto msgs = session.getContextMessages();
    std::string tail;
    for (auto it = msgs.rbegin(); it != msgs.rend(); ++it) {
        if (it->role == "system") continue; // the fixed prefix is not conversation
        std::string body = it->content;
        if (body.size() > kPerMessageCap) body = body.substr(0, kPerMessageCap) + " [...]";
        std::string line = it->role + ": " + body + "\n";
        if (tail.size() + line.size() > kSliceBudget) break;
        tail = line + tail; // keep chronological order
    }
    if (tail.empty() && local.empty()) return std::string();
    local += tail;

    // Small session: the digest IS the summary, no API call needed.
    if (local.size() < 6000) return local;

    // 3. Ask the model, with the compactor's own prompt. Read-only: nothing is
    //    applied to the session, so a skill run can never rewrite the conversation.
    ChatRequest req;
    req.model = model_;
    req.messages.push_back(Message::System(loadPromptFile("compactor.md")));
    req.messages.push_back(Message::User(local));
    req.stream = false;
    req.maxTokens = std::min(configMaxTokens, 2048);
    req.temperature = 0.0;
    std::string err;
    const ChatResponse resp = client.sendMessage(req, &err);
    if (!err.empty() || resp.messages.empty()) {
        debugLogWarnf("[Agent] shared-context summary failed (%s); using the local digest",
                      err.empty() ? "empty response" : err.c_str());
        return local;
    }
    const std::string s = resp.messages.back().content;
    if (s.size() < 50) {
        debugLogWarnf("[Agent] shared-context summary too short (%zu bytes); using the local "
                      "digest",
                      s.size());
        return local;
    }
    debugLogf("[Agent] shared context: %zu bytes of transcript -> %zu byte summary", local.size(),
              s.size());
    return s;
}

// ========== Tool execution ==========
ToolResult Agent::executeTool(const ToolCall& call) {
    ToolResult r;
    r.toolCallId = call.id;
    r.name = call.name;
    // Drop anything a previous call left behind on this thread before the tool
    // runs (the writing tools record into a thread_local slot). Reset before the
    // allowlist check as well, so the early returns stay clean.
    diff_capture::reset();
    if (!isToolAllowed(call.name)) {
        r.content = "Error: Tool '" + call.name + "' not allowed.";
        r.isError = true;
        return r;
    }
    r.content = toolRegistry.execute(call.name, call.arguments);
    r.isError = (r.content.compare(0, 6, "Error:") == 0);
    // A wish pass counts as progress when the model did anything at all -- reading,
    // grepping and running commands are work. Only a pass with no tool call is idle.
    if (call.name != "wish_block") {
        std::lock_guard<std::mutex> lk(wishMutex_);
        if (wish_.active) wish_.progressedThisPass = true;
    }
    if (toolRegistry.isCancelled()) r.isCancelled = true;
    // A file-writing tool leaves the unified diff of what it changed here. It
    // rides on the ToolResult and the tool row, never inside `content`.
    diff_capture::Entry captured = diff_capture::take();
    if (!captured.unified.empty()) r.diff = std::move(captured.unified);
    return r;
}

std::string Agent::commandOf(const ToolCall& call) {
    try {
        return nlohmann::json::parse(call.arguments).value("command", "");
    } catch (...) {
        return std::string();
    }
}

bool Agent::hasGrant(const std::string& command) const {
    if (command.empty()) return false;
    std::lock_guard<std::mutex> lk(approvalMutex_);
    return std::find(grants_.begin(), grants_.end(), command) != grants_.end();
}

delete_guard::Verdict Agent::evaluateCommandPolicy(const std::string& command) const {
    delete_guard::Verdict v;
    const std::string& cmd = command;
    if (cmd.empty()) return v;
    v = delete_guard::evaluate(cmd);
    if (v.decision == delete_guard::Decision::Ask && v.needsExistenceCheck) {
        // `cp a b` only overwrites when b is already there. The classifier is
        // pure, so the stat happens here; a relative path resolves against the
        // shell's working directory, which is the process CWD (the workspace).
        std::error_code ec;
        std::filesystem::path target(v.checkTarget);
        if (target.is_relative()) target = std::filesystem::current_path(ec) / target;
        if (!ec && !std::filesystem::exists(target, ec)) v.decision = delete_guard::Decision::Allow;
    }
    return v;
}

bool Agent::policyNeedsApproval(const delete_guard::Verdict& v, const std::string& command) const {
    if (v.decision != delete_guard::Decision::Ask) return false;
    // A command the user allowed forever never asks again, in any mode.
    if (hasGrant(command)) return false;
    switch (approvalMode()) {
    case ApprovalMode::Strict:
        // Every Ask asks. Nothing is offered to remember.
        return true;
    case ApprovalMode::Loose:
        // Only the red line (Deny) is refused; it never reaches this point.
        debugLogf("[Agent] approval_mode=loose: %s", command.c_str());
        return false;
    case ApprovalMode::Balanced:
    default:
        // Functional overwrites (`build > log.txt`) run; destructive ones ask.
        if (v.kind == delete_guard::Kind::Functional) {
            debugLogf("[Agent] approval_mode=balanced: %s", command.c_str());
            return false;
        }
        return true;
    }
}

bool Agent::policyNeedsApproval(const std::string& command) const {
    return policyNeedsApproval(evaluateCommandPolicy(command), command);
}

bool Agent::awaitApproval(const ToolCall& call) {
    {
        std::lock_guard<std::mutex> lk(approvalMutex_);
        pendingApproval_ = call;
        approvalDone_ = false;
        approvalGranted_ = false;
        approvalAction_ = 0;
    }
    setPhase(AgentPhase::AwaitApproval);
    updateSnapshot();
    std::unique_lock<std::mutex> lk(approvalMutex_);
    approvalCv_.wait(lk, [this]() { return approvalDone_ || cancelRequested_.load(); });
    const bool granted = approvalGranted_;
    pendingApproval_.reset();
    return granted;
}
// ========== Build request ==========

ChatRequest Agent::buildChatRequest() const {
    ChatRequest req;
    req.messages = session.getContextMessages();
    req.stream = true;
    req.model = model_;
    req.maxTokens = configMaxTokens;
    req.temperature = configTemperature;
    req.reasoningEffort = configReasoningEffort;
    req.tools = getFilteredToolDefinitions();
    if (!req.messages.empty()) {
        auto td = copyTodoData();
        std::string ur = buildUserRequestSystemMessage(td), tm = buildTodoSystemMessage(td),
                    ov = buildOverviewSystemMessage(td);
        std::vector<Message> inj;
        // P3: the capability map. Opaque text from the TUI, one string per turn.
        if (!capabilitiesBlock_.empty()) inj.push_back(Message::System(capabilitiesBlock_));
        const Wish w = wishSnapshot();
        if (w.active) inj.push_back(Message::System(wishSystemMessage(w)));
        if (!ur.empty()) inj.push_back(Message::System(ur));
        if (!tm.empty()) inj.push_back(Message::System(tm));
        if (!ov.empty()) inj.push_back(Message::System(ov));
        if (!inj.empty()) {
            // Insert right after the FIXED system block -- the same rule
            // Session::applyCompaction() uses for the compaction summary. P0 (role)
            // + P1 (environment) + P2 (project) then keep indices [0, sysCount), so
            // a changing todo or tool table never shifts them and DeepSeek's KV
            // prefix cache keeps hitting.
            size_t sysCount = 0;
            while (sysCount < req.messages.size() && req.messages[sysCount].role == "system")
                ++sysCount;
            req.messages.insert(req.messages.begin() + static_cast<std::ptrdiff_t>(sysCount),
                                std::make_move_iterator(inj.begin()),
                                std::make_move_iterator(inj.end()));
        }
    }
    return req;
}

bool Agent::isToolAllowed(const std::string& name) const {
    // Auto-registered tools (MCP) win first: they are not in the prompt and
    // never will be.
    if (autoToolNames_.count(name)) return true;
    // Then the old rule, unchanged: an empty allowlist means "no restriction".
    if (allowedTools_.empty()) return true;
    return std::find(allowedTools_.begin(), allowedTools_.end(), name) != allowedTools_.end();
}

void Agent::addAutoTools(const std::vector<std::string>& names) {
    for (const auto& n : names)
        if (!n.empty()) autoToolNames_.insert(n);
}

void Agent::removeAutoTools(const std::vector<std::string>& names) {
    for (const auto& n : names)
        autoToolNames_.erase(n);
}

std::vector<ToolDefinition> Agent::getFilteredToolDefinitions() const {
    auto all = toolRegistry.getToolDefinitions();
    // ToolRegistry iterates an unordered_map, so this order used to change when MCP
    // rehashed it -- which breaks the request prefix cache for no benefit. Sort
    // once here, at the single read point, instead of relying on the map.
    std::sort(all.begin(), all.end(),
              [](const ToolDefinition& a, const ToolDefinition& b) { return a.name < b.name; });
    // /wish tools exist only in /wish mode. Filtered, not unregistered: the registry
    // is read by the agent thread and written by syncTools on a turn boundary, so
    // registering and unregistering at runtime would need a lock it does not have.
    if (!wishSnapshot().active) {
        all.erase(std::remove_if(all.begin(), all.end(),
                                 [](const ToolDefinition& d) {
                                     return d.name == "wish_decide" || d.name == "wish_block";
                                 }),
                  all.end());
    }
    // Fast path: nothing is restricted and nothing was auto-registered, so the
    // whole registry goes out (the pre-MCP behaviour).
    if (allowedTools_.empty() && autoToolNames_.empty()) return all;
    std::vector<ToolDefinition> f;
    for (auto& d : all)
        if (isToolAllowed(d.name)) f.push_back(d);
    return f;
}

void Agent::repairSession() {
    session.repairOrphanedToolCalls();
}

// ========== Misc ==========

std::vector<Message> Agent::getNewMessagesSince(int64_t id) const {
    return (storage && storage->isOpen()) ? storage->queryMessages(id) : std::vector<Message>{};
}

void Agent::setModel(const std::string& m) {
    model_ = m;
    contextBudget = Session::computeContextBudget(effectiveContextWindow(), configMaxTokens);
}
Session::PressureLevel Agent::getContextPressure() const {
    return contextBudget.getPressure(getEstimatedContextTokens());
}

size_t Agent::getEstimatedContextTokens() const {
    if (contextTokensDirty_) {
        size_t t = 0;
        auto msgs = session.getContextMessages();
        for (auto& m : msgs) {
            t += Session::estimateTokens(m.content) + Session::estimateTokens(m.reasoningContent);
            t += Session::estimateImageTokens(m.images.size());
            for (auto& tc : m.toolCalls)
                t += Session::estimateTokens(tc.name) + Session::estimateTokens(tc.arguments);
        }
        cachedContextTokens_ = t;
        contextTokensDirty_ = false;
    }
    return cachedContextTokens_;
}

void Agent::setRequestParams(int mt, double t) {
    configMaxTokens = mt;
    configTemperature = t;
}

// ========== /wish ==========

void Agent::startWish(const std::string& objective) {
    std::lock_guard<std::mutex> lk(wishMutex_);
    wish_.reset(objective);
}

void Agent::cancelWish() {
    std::lock_guard<std::mutex> lk(wishMutex_);
    wish_.clear();
}

Wish Agent::wishSnapshot() const {
    std::lock_guard<std::mutex> lk(wishMutex_);
    return wish_;
}

Wish Agent::endWishPass() {
    std::lock_guard<std::mutex> lk(wishMutex_);
    if (!wish_.active) return wish_;
    // Remember that a checklist existed: update_todo(done) empties it, and an empty
    // TODO must only mean "finished" once there was something to finish.
    if (hasUnfinishedTodo()) wish_.sawTodo = true;
    ++wish_.passes;
    if (wish_.progressedThisPass)
        wish_.idleRounds = 0;
    else
        ++wish_.idleRounds;
    wish_.progressedThisPass = false;
    return wish_;
}

bool Agent::hasUnfinishedTodo() const {
    // Read the data, not the panel: F9 can hide the TODO view while the checklist
    // is still the definition of done.
    std::lock_guard<std::mutex> lk(todoMutex);
    return !todoData.pendingTodo.empty();
}

std::string Agent::unfinishedTodoText() const {
    std::lock_guard<std::mutex> lk(todoMutex);
    return todoData.pendingTodo;
}

std::vector<std::string> Agent::wishDecisions() const {
    std::lock_guard<std::mutex> lk(todoMutex);
    return todoData.decisions;
}

std::string Agent::wishStopReason(const Wish& w) const {
    if (!w.active) return std::string();
    if (w.blocked) return "blocked: " + w.blockReason;
    if (w.sawTodo && !hasUnfinishedTodo()) return std::string("todo:done");
    if (w.idleRounds >= wish_mode::kMaxIdleRounds)
        return "idle: two passes without a single tool call";
    if (w.passes >= wish_mode::kMaxPasses)
        return "passes: " + std::to_string(w.passes) + " (the ceiling)";
    return std::string();
}

std::string Agent::wishSystemMessage(const Wish& w) {
    if (!w.active) return std::string();
    return std::string(WISH_SYSTEM_PREFIX) +
           "Mode: decide and proceed. This overrides the normal \"ask when ambiguous\" rule.\n"
           "Wish: " +
           w.objective +
           "\n"
           "- Step 1: call update_todo(init) with a full checklist. That list IS the "
           "definition of done; every part of the wish needs its own line.\n"
           "- When a choice would change what you build (language, data model, public API, "
           "destructive step), do NOT ask. Pick the most defensible option, call "
           "wish_decide, and continue in the same turn.\n"
           "- Never end a turn with a question. If you truly cannot proceed, call "
           "wish_block.\n"
           "- The wish is done when every TODO line is marked done.\n";
}

bool Agent::handleQuickCommand(const std::string& input) {
    std::string c = input;
    auto s = c.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) return false;
    auto e = c.find_last_not_of(" \t\r\n");
    c = c.substr(s, e - s + 1);
    if (c == "/clear") {
        clearSession();
        addPersistedMessage(Message::Assistant(SESSION_CLEARED));
        return true;
    }
    if (c == "/help") {
        addPersistedMessage(Message::Assistant(QUICK_HELP));
        return true;
    }
    if (c == "/undo" || c == "/undo!") {
        const bool force = (c == "/undo!");
        const snapshot::Report r = snapshot::undoLast(sessionId(), force);
        addPersistedMessage(Message::System(snapshotReportText("/undo", r)));
        return true;
    }
    if (c.rfind("/restore", 0) == 0) {
        std::istringstream iss(c.substr(std::string("/restore").size()));
        int turn = -1;
        iss >> turn;
        bool confirm = false;
        std::string word;
        while (iss >> word)
            if (word == "confirm") confirm = true;
        if (turn < 0) {
            addPersistedMessage(
                Message::System("[Snapshot] Usage: /restore <turn> confirm -- see /help."));
            return true;
        }
        const snapshot::Report r = snapshot::restoreTo(sessionId(), turn, confirm);
        addPersistedMessage(Message::System(snapshotReportText("/restore", r)));
        return true;
    }
    return false;
}
