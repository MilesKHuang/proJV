// proJV TUI -- application shell (backend wiring, mirroring legacy App without ImGui).
#pragma once

#include "models.h"
#include "core/config.h"
#include "core/storage.h"
#include "client/deepseek.h"
#include "core/agent.h"
#include "core/skill_runner.h"
#include "tools/registry.h"
#include "tools/python_tool_manager.h"
#include "tui/bubble_model.h"
#include "tui/status_bar.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

class IProcessRunner;

class TuiApp {
  public:
    TuiApp();
    ~TuiApp();

    bool initialize(IProcessRunner* procRunner);
    bool hasApiKey() const { return config.loaded && !config.apiKey.empty(); }

    Agent* getAgent() { return agent; }

    // Live agent status / phase (forwarded for streaming rendering).
    AgentStatus getStatus() const { return agent ? agent->getStatus() : AgentStatus{}; }
    AgentPhase getPhase() const { return agent ? agent->getPhase() : AgentPhase::Idle; }

    // Snapshot of the status-bar inputs.
    status_bar::Data getStatusBarData() const;

    // TODO panel data.
    TodoData copyTodoData() const { return agent ? agent->copyTodoData() : TodoData{}; }

    // Config access / persistence / approval.
    const AppConfig& getConfig() const { return config; }
    void saveApiKeyAndConnect(const std::string& apiKey);
    void saveFullConfig(const std::string& apiKey, const std::string& baseUrl, int maxTokens,
                        double temperature, const std::string& workspace,
                        const std::string& compiler, const std::string& python,
                        const std::string& model);
    void approveTool(int action) {
        if (agent) agent->approveTool(action);
    }

    // System prompt role switching (coder / designer / analyzer / ...).
    const std::vector<std::string>& promptFiles() const { return promptFiles_; }
    int activePromptIndex() const { return activePromptIndex_; }
    void switchPrompt(int index);

    // Switch the model (persists to config and resets context auto-detect).
    void setModel(const std::string& model);

    // Session management (new chat / switch dialog / save copy).
    void newChat();
    void switchToDialog(const std::string& dbPath);
    std::vector<std::string> listSessions() const;

    // Toggle the global reasoning expanded/collapsed state (all history).
    void toggleReasoning();
    bool reasoningExpanded() const { return reasoningExpanded_; }

    // Best-effort shutdown for exit signals: cancel agent, close DB quickly
    // without joining the models thread (which never touches the DB).
    void emergencyShutdown();

    // Keyboard scroll for the chat area (row-granular, real rendered rows).
    // delta > 0 scrolls up (toward older content); delta < 0 scrolls down.
    void scrollChat(int delta);
    void resetChatScroll();
    // Called by the renderer after layout with the chat's real rendered row count.
    void setChatContentRows(int rows) { chatContentRows_ = rows; }
    // Called by the renderer after layout with the chat's visible (viewport) rows.
    void setChatViewportRows(int rows) { chatViewportRows_ = rows; }
    int chatFocusRow() const;

    // Incremental sync: pull new DB messages into chatHistory (call per frame).
    void syncChatFromAgent();

    const std::vector<bubble_model::Bubble>& bubbles() const { return chatHistory; }

    // Queue a user message and start an agent turn on a background thread.
    void sendMessage(const std::string& text);
    bool isBusy() const { return agentThreadRunning_.load(); }
    void cancelTurn();

    // Run a JSON-configured skill workflow on a background thread.
    // `rawInput` is the exact user line (e.g. "/monica D:\dir"); it is what gets
    // persisted so history replays match what the user typed. Empty -> the old
    // "/skill <name> <topic>" form is synthesized.
    void startSkill(const std::string& name, const std::string& topic,
                    const std::string& rawInput = std::string());
    std::string getSkillStatus() const;
    // Snapshot of the live skill agent pool for the sidebar (see START/agents).
    std::vector<SkillRunner::AgentSnapshot> skillAgentStatuses() const;
    size_t pendingCount() const { return pendingQueue_.size(); }

    // Invoked on the agent thread when a turn completes (used to trigger a redraw).
    std::function<void()> onTurnComplete;

    // Invoked on the agent thread on every streamed token update.
    std::function<void()> onStreamingTick;

    // Invoked when the theme changes (used to trigger a redraw).
    std::function<void()> onThemeChanged;

    // Model list (fetched from the API on a background thread).
    void refreshModels();
    std::vector<ModelInfo> getAvailableModels() const;
    bool modelsLoading() const { return modelsLoading_.load(); }
    std::function<void()> onModelsUpdated;

    // Cancel agent + join threads (call before exit).
    void shutdown();

  private:
    void setupTools();
    void harvestSpeakers();
    void buildBubblesFromMessages();
    // Inject the fixed workspace context as system messages at the start of a
    // conversation: the directory tree, then the repository's AGENTS.md. Empty
    // pieces are skipped. Called on startup, on F3 and on "/clear".
    void injectWorkspaceContext();
    // Body shared by newChat() and "/clear": cancel the turn, wipe the session,
    // then re-inject the system prompt plus the workspace context. keepStorage
    // false also drops the session database and starts a new one (F3); "/clear"
    // keeps the same session file, so history rows stay where they are.
    void reinitConversation(bool keepStorage);

    // Agent thread body: one user turn, then -- if run_skill asked for it -- the
    // skill phase, then a follow-up turn so the model can report the result.
    void runAgentTurnLoop();

    // Runs one installed skill to completion on the calling thread. Narration and
    // the final document are persisted as assistant messages, which is exactly
    // what an explicit "/<skill> <topic>" trigger does: both go through here.
    // `rawInput` is the user line persisted for replay; empty means the run was
    // requested by the agent (run_skill) and no user bubble is invented.
    bool runSkillBlocking(const std::string& name, const std::string& topic,
                          const std::string& rawInput);

    void launchAgentThread();
    void joinAgentThread();
    void joinModelsThread();
    void joinSkillThread();
    void drainPendingQueue();

    // Skill helpers. buildSharedContext() renders the main session into the
    // "[SHARED SESSION CONTEXT]" board both skill paths feed to SkillRunner.
    std::string buildSharedContext() const;

    AppConfig config;
    DeepSeekClient client;
    ToolRegistry tools;
    std::optional<PythonToolManager> pytoolMgr_;
    Storage storage;
    Agent* agent = nullptr;
    IProcessRunner* procRunner_ = nullptr;

    std::vector<bubble_model::Bubble> chatHistory;
    int64_t lastMessageId_ = 0;
    // Registry of active skill role display names. bubble_model::deriveBubbles
    // tags a "**[<name>]** " prefix as a speaker only when <name> is in here.
    std::set<std::string> knownSpeakers_;
    int chatScrollRow_ = 0;        // real rendered-row target (0 = top)
    bool chatFollowBottom_ = true; // stick to the newest content
    int chatContentRows_ = 0;      // real rendered rows from the last layout
    int chatViewportRows_ = 0;     // visible rows from the last layout
    std::string sessionsDir_;
    std::vector<std::string> promptFiles_;
    int activePromptIndex_ = 0;

    std::vector<std::string> pendingQueue_;
    bool reasoningExpanded_ = false; // global chain-of-thought collapse state (default collapsed)

    std::vector<ModelInfo> availableModels_;
    mutable std::mutex modelsMutex_;
    std::atomic<bool> modelsLoading_{false};
    std::thread modelsThread_;

    std::thread agentThread_;
    std::atomic<bool> agentThreadRunning_{false};

    // Skill requested by the run_skill tool during the current turn. Written by
    // the executor (agent thread), taken by runAgentTurnLoop (same thread) and
    // dropped by cancelTurn (TUI thread) -- all under skillMutex_.
    struct PendingSkill {
        std::string name;
        std::string topic;
    };
    std::optional<PendingSkill> pendingSkill_;
    int skillCallsThisTurn_ = 0; // hard cap per user turn (run_skill loop guard)
    // Last skill that already ran in this user turn. Re-running it is refused:
    // the model may read the produced files, but not redo the whole workflow.
    std::string lastSkillThisTurn_;

    std::unique_ptr<SkillRunner> skillRunner_;
    std::string skillsDir_;
    std::thread skillThread_;
    std::string skillStatus_;
    mutable std::mutex skillMutex_;
    std::atomic<int> totalPromptTokens_{0};
    std::atomic<int> totalCompletionTokens_{0};
};
