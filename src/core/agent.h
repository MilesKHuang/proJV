#pragma once
#include "models.h"
#include "session.h"
#include "storage.h"
#include "delete_guard.h"
#include "client/deepseek.h"
#include "tools/registry.h"
#include "tools/todo_tool.h"
#include "wish.h"
#include "tool_list_parser.h"
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <set>
#include <vector>
#include <optional>

enum class AgentPhase { Idle, Streaming, ExecutingTools, AwaitApproval, Error };

class Agent {
  public:
    Agent(DeepSeekClient& client, ToolRegistry& tools);
    ~Agent();

    void setStorage(Storage* s) { storage = s; }
    std::function<void(std::function<void()>)> enqueueWrite;
    std::function<void()> ensureStorage;
    void addPersistedMessage(const Message& msg);
    void loadFromStorage();

    // `images` are vision attachments parsed from the user's "@path" markers.
    // They are persisted with the message row (paths only); an empty vector
    // keeps the plain-text behavior.
    void startTurn(const std::string& text, std::vector<ImageAttachment> images = {});
    void run();

    // One-shot System message injected at the start of the NEXT turn. Used after
    // a skill phase to tell the model "the skill finished, report it now".
    // Persisted as a System message (invisible in the chat), so it survives the
    // HTTP retry path and is visible in later turns too.
    void setTurnNudge(const std::string& nudge) { turnNudge_ = nudge; }

    void approveTool(int action);

    // The single exec_shell call currently waiting for the user, or nullopt.
    // The approval dialog must read this and nothing else: the session copy
    // happens to hold the same batch, but it is not the source of truth and
    // reading it couples the UI to whatever the agent thread last wrote.
    std::optional<ToolCall> pendingApproval() const;
    void cancel();
    AgentStatus getStatus() const;
    AgentPhase getPhase() const { return phase_.load(); }
    const Session& getSession() const { return session; }
    void clearSession();
    void newTurn();
    void requestCompact() { compactRequested_.store(true); }

    Session::PressureLevel getContextPressure() const;
    size_t getEstimatedContextTokens() const;
    size_t getContextWindowSize() const { return contextBudget.windowTokens; }

    bool handleQuickCommand(const std::string& input);
    bool compactSession(bool force = false);

    void setModel(const std::string& model);
    void setRequestParams(int maxTokens, double temperature);
    // config.toml "approval_mode" (F2): how much the agent asks. Deny is outside
    // the dial -- formatting/partitioning never runs, whatever the mode.
    // This is a deliberate mirror of AppConfig::approvalMode, not a second source of
    // truth: the agent thread reads it on every tool call, so it has to be atomic,
    // and AppConfig (a plain struct owned by the TUI) cannot be. TuiApp pushes it in
    // applyConfigToAgent(), the same way maxTokens and model are pushed.
    void setApprovalMode(ApprovalMode m) { approvalMode_.store(m, std::memory_order_release); }
    ApprovalMode approvalMode() const { return approvalMode_.load(std::memory_order_acquire); }
    // "ALWAYS" is only offered outside Strict mode.
    bool canAlways() const { return approvalMode() != ApprovalMode::Strict; }
    // Commands allowed forever, loaded from config.toml at startup.
    void setPersistentGrants(const std::vector<std::string>& grants) {
        std::lock_guard<std::mutex> lk(approvalMutex_);
        grants_ = grants;
    }
    std::vector<std::string> grants() const {
        std::lock_guard<std::mutex> lk(approvalMutex_);
        return grants_;
    }
    void clearGrants() {
        std::lock_guard<std::mutex> lk(approvalMutex_);
        grants_.clear();
    }
    // Called on the agent thread when the user clicked ALWAYS, so the TUI can write
    // the grant into config.toml. Persistence lives in the TUI: core has no config.
    std::function<void(const std::string&)> onGrantPersist;
    // Would this command text reach the approval dialog right now? The turn loop
    // and the tests both go through this, so the rule has exactly one home.
    bool policyNeedsApproval(const std::string& command) const;

    // A bounded digest of this conversation for a skill's shared context.
    // Skills used to receive the WHOLE transcript: in a long session that is hundreds
    // of thousands of tokens, and every sub-agent carried all of it. This reuses the
    // compactor's shape (same prompt, same "overview + todo + transcript slice" input),
    // is read-only -- it never applies the compaction -- and falls back to the local
    // digest (user request + TODO + overview + last turns) whenever the model call
    // fails, so a skill never fails because summarising did.
    std::string summarizeConversationForSharing();
    // Reasoning effort (official enum: none/low/high/max). Anything unusable
    // falls back to the official default "high".
    void setReasoningEffort(const std::string& effort) {
        const std::string v = normalizeReasoningEffort(effort);
        configReasoningEffort = v.empty() ? kReasoningEffortOfficialDefault() : v;
    }
    void setSystemPrompt(const std::string& sp) {
        systemPrompt_ = sp;
        reloadAllowedTools();
    }
    // Replace the first system message in session with a new prompt.
    // Keeps all other messages intact -- used for role switching without clearing context.
    void replaceSystemPrompt(const std::string& newPrompt);

    // P3, the capability map. Built by the TUI (it owns skill and MCP metadata),
    // stored here as opaque text: the Agent never parses it, rewrites it or
    // regenerates it, it only inserts it before the per-turn block. One turn
    // means one string, so the two buildChatRequest() calls of a turn (normal +
    // HTTP retry) send identical bytes.
    void setCapabilitiesBlock(std::string block) { capabilitiesBlock_ = std::move(block); }

    // Refresh the persisted [ENVIRONMENT] block in place (it is P1, so a config
    // save that changes a toolchain path would otherwise stay stale until F3).
    // Returns false when no environment message exists yet.
    bool replaceEnvironmentBlock(const std::string& text);

    // --- Tool visibility (two orthogonal channels) ----------------------
    //   allowedTools_  -- parsed from the "tools available:" line of the role
    //                     prompt. Empty means "no restriction": the old escape
    //                     hatch, kept byte-for-byte.
    //   autoToolNames_ -- tools that become visible by being registered. MCP
    //                     tool names only exist at runtime (mcp__fs__read_file),
    //                     so a user cannot pre-write them into a prompt.
    // They must stay separate: pushing MCP names into allowedTools_ would make
    // it non-empty and silently disable every builtin for a user who deleted the
    // tools line on purpose. Query-time only -- never cached, because there are
    // three setSystemPrompt/replaceSystemPrompt call sites.
    bool isToolAllowed(const std::string& name) const;
    void addAutoTools(const std::vector<std::string>& names);
    void removeAutoTools(const std::vector<std::string>& names);
    size_t autoToolCount() const { return autoToolNames_.size(); }
    // Read-only view of the role allowlist, for the capability map (P3). It must
    // be the same list getFilteredToolDefinitions() applies, or the map would
    // describe a different tool set than the request carries.
    const std::vector<std::string>& getAllowedTools() const { return allowedTools_; }
    void setWorkspacePath(const std::string& ws) { workspacePath_ = ws; }
    void setContextWindow(size_t window) {
        configContextWindow = window;
        contextBudget = Session::computeContextBudget(effectiveContextWindow(), configMaxTokens);
    }

    std::function<void(int prompt, int completion)> onTokenUsage;
    // Called (from the agent thread) whenever streamed reasoning/content
    // updates the status snapshot. The TUI uses it to post a redraw event.
    std::function<void()> onStreamingTick;
    // Called (from the agent thread) on every loop phase change, so the TUI can
    // mirror the state into the hosting terminal (OSC 7501). `msg` is a short,
    // single-line, human-readable summary; empty means "use the phase default".
    // core/ must not depend on tui/, so this is a plain callback.
    void setPhaseCallback(std::function<void(AgentPhase, const std::string&)> cb) {
        phaseCallback_ = std::move(cb);
    }
    Session::ContextBudget contextBudget;

    TodoData copyTodoData() const {
        std::lock_guard<std::mutex> lock(todoMutex);
        return todoData;
    }

    std::vector<Message> getNewMessagesSince(int64_t sinceId) const;

    // --- /wish mode ------------------------------------------------------
    // A wish keeps the turn loop going until the TODO list is empty. The state is
    // deliberately small: the objective, the TODO and the overview are already
    // injected every turn, so a wish only needs a switch and three counters.
    void startWish(const std::string& objective);
    void cancelWish();
    Wish wishSnapshot() const;
    // Called by the TUI once per completed pass: rolls progressedThisPass into
    // idleRounds and counts the pass. Returns the snapshot after the roll.
    Wish endWishPass();
    bool hasUnfinishedTodo() const;
    // Remaining TODO lines, for the "wish stopped" report.
    std::string unfinishedTodoText() const;
    std::vector<std::string> wishDecisions() const;
    // Why the loop must stop, given the current wish state ("" = keep going).
    std::string wishStopReason(const Wish& w) const;

    // The tools this request carries: registry minus the role allowlist minus the
    // /wish tools while no wish is active. Public so tests can assert the wish
    // filter -- it is the only thing standing between the model and a tool that
    // would write decisions nobody asked for.
    std::vector<ToolDefinition> getFilteredToolDefinitions() const;

  private:
    // The [WISH] system message, inserted after P3 while a wish is active.
    static std::string wishSystemMessage(const Wish& w);
    // Verdict for one command text, existence check included.
    delete_guard::Verdict evaluateCommandPolicy(const std::string& command) const;
    // Does this verdict still need the user's click?
    bool policyNeedsApproval(const delete_guard::Verdict& v, const std::string& command) const;

  private:
    void reloadAllowedTools() { allowedTools_ = parseAvailableTools(systemPrompt_); }
    StreamCallbacks makeCallbacks();
    ChatRequest buildChatRequest() const;
    void doCompaction(bool force = false);
    void checkContextWarning();
    ToolResult executeTool(const ToolCall& call);

    // Shell-command policy. Red lines are refused outright; destructive
    // commands are asked one exact command at a time (see delete_guard.h).
    bool awaitApproval(const ToolCall& call);
    bool hasGrant(const std::string& command) const;
    static std::string commandOf(const ToolCall& call);
    // Session identity for the snapshot index (database file name, or a
    // per-process fallback before the database exists).
    std::string sessionId() const;
    void repairSession();
    void updateSnapshot();
    // Single writer for phase_: keeps the loop and the OSC 7501 report in sync.
    void setPhase(AgentPhase p, const std::string& msg = std::string());
    std::string approvalSummary() const;
    size_t effectiveContextWindow() const {
        if (configContextWindow > 0) return configContextWindow;
        return contextWindowForModel(model_);
    }

    DeepSeekClient& client;
    ToolRegistry& toolRegistry;
    Storage* storage = nullptr;
    Session session;

    std::atomic<AgentPhase> phase_{AgentPhase::Idle};
    AgentStatus status_;
    mutable std::mutex snapshotMutex_;
    std::function<void(AgentPhase, const std::string&)> phaseCallback_;

    std::string currentContent_;
    std::string currentReasoning_;
    std::vector<ToolCall> currentToolCalls_;
    bool streamFinished_ = false;
    bool streamError_ = false;
    std::string streamErrorMsg_;
    std::string lastFinishReason_;
    int reasoningLengthRetries_ = 0;
    int streamPromptTokens_ = 0;
    int streamCompletionTokens_ = 0;

    std::vector<ToolCall> pendingToolCalls_;
    int toolIndex_ = 0;
    std::vector<ToolResult> toolResults_;
    // No round-level tool-call counter on purpose: it never stopped a turn, it only
    // injected noise every 30 calls. Runaway loops are caught by the consecutive
    // failure check (two errors in a row) and by doCompaction().
    // Approval state. One call at a time: the grant it produces is bound to the
    // exact command text, so the dialog and the wait can never cover a batch.
    std::optional<ToolCall> pendingApproval_;
    std::vector<std::string> grants_; // exact exec_shell command texts allowed for the session
    bool approvalDone_ = false;
    bool approvalGranted_ = false;
    int approvalAction_ = 0;
    mutable std::mutex approvalMutex_;
    std::condition_variable approvalCv_;

    mutable size_t cachedContextTokens_ = 0;
    mutable bool contextTokensDirty_ = true;

    std::atomic<bool> cancelRequested_{false};
    std::atomic<bool> compactRequested_{false};
    std::atomic<ApprovalMode> approvalMode_{ApprovalMode::Balanced};
    TodoData todoData;
    mutable std::mutex todoMutex;

    // /wish state. Guarded by wishMutex_: the agent thread sets progressedThisPass
    // and the blocked flag from inside tools, the TUI reads and rolls passes from
    // the turn loop, and /wish cancel comes from the input thread.
    Wish wish_;
    mutable std::mutex wishMutex_;

    std::string turnNudge_;
    std::string model_ = "deepseek-v4-flash";
    std::string workspacePath_;
    std::string systemPrompt_;
    std::string capabilitiesBlock_;
    std::vector<std::string> allowedTools_;
    // Tool names that are visible because they were registered at runtime (MCP).
    // Written only at a turn boundary, on the agent thread, by McpManager.
    std::set<std::string> autoToolNames_;
    size_t configContextWindow = 0;
    int configMaxTokens = 65536;
    double configTemperature = 0.0;
    std::string configReasoningEffort = "max";
};
