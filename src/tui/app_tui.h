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
                        const std::string& compiler, const std::string& python);
    void approveTool(int action) { if (agent) agent->approveTool(action); }

    // Session management (new chat / switch dialog / save copy).
    void newChat();
    void switchToDialog(const std::string& dbPath);
    bool saveDialogToFile(const std::string& filename);
    const std::string& currentSessionPath() const { return storage.currentPath(); }

    // Incremental sync: pull new DB messages into chatHistory (call per frame).
    void syncChatFromAgent();

    const std::vector<bubble_model::Bubble>& bubbles() const { return chatHistory; }

    // Queue a user message and start an agent turn on a background thread.
    void sendMessage(const std::string& text);

    // Invoked on the agent thread when a turn completes (used to trigger a redraw).
    std::function<void()> onTurnComplete;

    // Cancel agent + join threads (call before exit).
    void shutdown();

private:
    void setupTools();
    void buildBubblesFromMessages();
    void launchAgentThread();
    void joinAgentThread();

    AppConfig config;
    DeepSeekClient client;
    ToolRegistry tools;
    std::optional<PythonToolManager> pytoolMgr_;
    Storage storage;
    Agent* agent = nullptr;
    IProcessRunner* procRunner_ = nullptr;

    std::vector<bubble_model::Bubble> chatHistory;
    int64_t lastMessageId_ = 0;
    std::string sessionsDir_;
    std::vector<std::string> promptFiles_;
    int activePromptIndex_ = 0;

    std::thread agentThread_;
    std::atomic<bool> agentThreadRunning_{false};
    std::atomic<int> totalPromptTokens_{0};
    std::atomic<int> totalCompletionTokens_{0};
};
