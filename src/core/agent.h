#pragma once
#include "models.h"
#include "session.h"
#include "storage.h"
#include "tool_worker.h"
#include "client/deepseek.h"
#include "tools/registry.h"
#include "tools/todo_tool.h"
#include "tool_list_parser.h"
#include <functional>
#include <thread>
#include <atomic>

class Agent {
public:
    Agent(DeepSeekClient& client, ToolRegistry& tools);

    // -- SQLite persistence (Agent level, not Session level) --
    void setStorage(Storage* s) { storage = s; }

    // Callback to enqueue write operations on the StorageWriteQueue.
    // The write queue serializes all SQLite writes to a single worker thread.
    // Set by App::initialize(). If not set, addPersistedMessage writes directly.
    std::function<void(std::function<void()>)> enqueueWrite;

    // 懒初始化存储回调：由 App 设置，在 sendMessage 首次调用时触发 DB 创建。
    // 避免启动时立即创建无内容的 DB 文件。
    std::function<void()> ensureStorage;

    // Add a message to both session (memory) and storage (SQLite)
    void addPersistedMessage(const Message& msg);

    // Load all messages from storage into session (replaces current)
    void loadFromStorage();

    // Send user message, start streaming response
    void sendMessage(const std::string& text);

    // Approve/reject pending tool call. action: 0=reject, 1=once, 2=always allow session
    void approveTool(int action);

    // Cancel current operation
    void cancel();

    // Get current status (thread-safe)
    AgentStatus getStatus() const;

    // Get session (read-only access)
    const Session& getSession() const { return session; }

    // Clear session
    void clearSession();

    // Context budget & pressure (Item 5: dynamic context awareness)
    Session::PressureLevel getContextPressure() const;
    size_t getEstimatedContextTokens() const;
    size_t getContextWindowSize() const { return contextBudget.windowTokens; }

    // Handle quick commands (e.g. /help, /clear, /save, /load, /doctor)
    bool handleQuickCommand(const std::string& input);

    // LLM-driven semantic compaction: plan → ask LLM for summary → apply.
    // Returns true if compaction was performed.
    bool compactSession();

    bool isSaveRequested() const { return saveRequested; }
    bool isLoadRequested() const { return loadRequested; }
    void clearSaveRequested() { saveRequested = false; }
    void clearLoadRequested() { loadRequested = false; }

    void setModel(const std::string& model);
    void setToolPaths(const std::string& cppCompilerPath, const std::string& pythonPath);
    void setRequestParams(int maxTokens, double temperature);
    // Set the system prompt text and reload the allowed-tools whitelist.
    void setSystemPrompt(const std::string& sp) {
        systemPrompt_ = sp;
        reloadAllowedTools();
    }
    void setWorkspacePath(const std::string& ws) { workspacePath_ = ws; }
    void setContextWindow(size_t window) {
        configContextWindow = window;
        contextBudget = Session::computeContextBudget(effectiveContextWindow(), configMaxTokens);
    }

    std::function<void(int prompt, int completion)> onTokenUsage;

    // Context budget (initialized in constructor / setModel)
    Session::ContextBudget contextBudget;

    TodoData copyTodoData() const {
        std::lock_guard<std::mutex> lock(todoMutex);
        return todoData;
    }
    // Incremental message access (for UI, replaces bubble table)
    // Returns messages with id > sinceId, ordered by id.
    std::vector<Message> getNewMessagesSince(int64_t sinceId) const;

private:
    DeepSeekClient& client;
    ToolRegistry& toolRegistry;
    Storage* storage = nullptr;
    Session session;
    AgentStatus status;

    mutable std::mutex statusMutex;

    bool saveRequested = false;
    bool loadRequested = false;

    std::string currentContent;
    std::string currentReasoning;
    std::vector<ToolCall> pendingToolCalls;

    std::vector<ToolCall> pendingApprovalCalls;
    std::atomic<bool> destructiveApproved{false};

    TodoData todoData;
    mutable std::mutex todoMutex;

    // -- Tool worker manager (replaces raw std::thread + detach) -------
    // Manages tool execution lifecycle: RAII thread, cancellation,
    // error detection cache. Results are returned via callback.
    ToolWorkerManager toolWorkerMgr_;

    // Cancellation guard: set by cancel(), checked in callbacks to prevent
    // onFinish from re-launching work after the user cancelled.
    std::atomic<bool> cancelled_{false};

    // Operation generation: incremented on cancel().  Tool worker callbacks
    // capture the generation at launch time; if it differs when they fire,
    // the results belong to a stale operation and are discarded.
    std::atomic<int> operationGen_{0};

    // Tool recursion depth guard (atomic for cross-thread access)
    std::atomic<int> toolCallDepth{0};
    int maxToolCallsPerRound = 30;

    // -- Config / params --
    std::string model_ = "deepseek-v4-flash";
    std::string cppCompilerPath;
    std::string pythonPath;
    std::string workspacePath_;
    std::string systemPrompt_;  // current system prompt text (for tool whitelist)
    std::vector<std::string> allowedTools_;  // parsed from system prompt by setSystemPrompt()
    size_t configContextWindow = 0;  // 0 = auto-detect from model name
    int configMaxTokens = 8192;
    double configTemperature = 0.0;

    // Effective context window after auto-detection (set in buildChatRequest)
    size_t effectiveContextWindow() const {
        if (configContextWindow > 0) return configContextWindow;
        return contextWindowForModel(model_);
    }

    StreamCallbacks makeCallbacks();
    void checkContextWarning();
    void sendToAPI();
    void queueOrExecuteToolCalls(const std::vector<ToolCall>& calls);
    ToolResult executeTool(const ToolCall& call);

    // Called by ToolWorkerManager when all tools complete.
    // Adds tool result messages, runs error detection, continues to API.
    void onToolResults(const std::vector<ToolResult>& results);

    void continueWithToolResults(const std::vector<ToolResult>& results);
    ChatRequest buildChatRequest() const;
    // Reload allowedTools_ by parsing systemPrompt_.
    void reloadAllowedTools() {
        allowedTools_ = parseAvailableTools(systemPrompt_);
    }
    // Returns tool definitions filtered to only those in allowedTools_.
    // Returns all tools if allowedTools_ is empty (backward compat).
    std::vector<ToolDefinition> getFilteredToolDefinitions() const;
    void repairSession();
    void setStatus(AgentState state, const std::string& msg = "");
    void setError(const std::string& err);
};
