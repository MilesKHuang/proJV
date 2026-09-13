// proJV TUI -- application shell (backend wiring, mirroring legacy App without ImGui).
#pragma once

#include "models.h"
#include "core/config.h"
#include "core/storage.h"
#include "client/deepseek.h"
#include "core/agent.h"
#include "tools/registry.h"
#include "tools/python_tool_manager.h"
#include "tui/bubble_model.h"
#include "tui/status_bar.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
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
    void saveFullConfig(const std::string& apiKey, const std::string& baseUrl,
                        int maxTokens, double temperature, const std::string& workspace,
                        const std::string& compiler, const std::string& python,
                        const std::string& model);
    void approveTool(int action) { if (agent) agent->approveTool(action); }

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
    void buildBubblesFromMessages();
    void launchAgentThread();
    void joinAgentThread();
    void joinModelsThread();
    void drainPendingQueue();

    AppConfig config;
    DeepSeekClient client;
    ToolRegistry tools;
    std::optional<PythonToolManager> pytoolMgr_;
    Storage storage;
    Agent* agent = nullptr;
    IProcessRunner* procRunner_ = nullptr;

    std::vector<bubble_model::Bubble> chatHistory;
    int64_t lastMessageId_ = 0;
    int chatScrollRow_ = 0;        // real rendered-row target (0 = top)
    bool chatFollowBottom_ = true; // stick to the newest content
    int chatContentRows_ = 0;      // real rendered rows from the last layout
    int chatViewportRows_ = 0;     // visible rows from the last layout
    std::string sessionsDir_;
    std::vector<std::string> promptFiles_;
    int activePromptIndex_ = 0;

    std::vector<std::string> pendingQueue_;
    bool reasoningExpanded_ = false;  // global chain-of-thought collapse state (default collapsed)

    std::vector<ModelInfo> availableModels_;
    mutable std::mutex modelsMutex_;
    std::atomic<bool> modelsLoading_{false};
    std::thread modelsThread_;

    std::thread agentThread_;
    std::atomic<bool> agentThreadRunning_{false};
    std::atomic<int> totalPromptTokens_{0};
    std::atomic<int> totalCompletionTokens_{0};
};
