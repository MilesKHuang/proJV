#pragma once
#include "models.h"
#include "session.h"
#include "storage.h"
#include "client/deepseek.h"
#include "tools/registry.h"
#include "tools/todo_tool.h"
#include "tool_list_parser.h"
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <vector>

enum class AgentPhase {
    Idle,
    Streaming,
    ExecutingTools,
    AwaitApproval,
    Error
};

class Agent {
public:
    Agent(DeepSeekClient& client, ToolRegistry& tools);
    ~Agent();

    void setStorage(Storage* s) { storage = s; }
    std::function<void(std::function<void()>)> enqueueWrite;
    std::function<void()> ensureStorage;
    void addPersistedMessage(const Message& msg);
    void loadFromStorage();

    void startTurn(const std::string& text);
    void run();

    void approveTool(int action);
    void cancel();
    AgentStatus getStatus() const;
    AgentPhase getPhase() const { return phase_.load(); }
    const Session& getSession() const { return session; }
    void clearSession();
    void newTurn();

    Session::PressureLevel getContextPressure() const;
    size_t getEstimatedContextTokens() const;
    size_t getContextWindowSize() const { return contextBudget.windowTokens; }

    bool handleQuickCommand(const std::string& input);
    bool compactSession();

    bool isSaveRequested() const { return saveRequested; }
    bool isLoadRequested() const { return loadRequested; }
    void clearSaveRequested() { saveRequested = false; }
    void clearLoadRequested() { loadRequested = false; }

    void setModel(const std::string& model);
    void setToolPaths(const std::string& cpp, const std::string& python);
    void setRequestParams(int maxTokens, double temperature);
    void setSystemPrompt(const std::string& sp) {
        systemPrompt_ = sp;
        reloadAllowedTools();
    }
    // Replace the first system message in session with a new prompt.
    // Keeps all other messages intact -- used for role switching without clearing context.
    void replaceSystemPrompt(const std::string& newPrompt);
    void setWorkspacePath(const std::string& ws) { workspacePath_ = ws; }
    void setContextWindow(size_t window) {
        configContextWindow = window;
        contextBudget = Session::computeContextBudget(effectiveContextWindow(), configMaxTokens);
    }

    std::function<void(int prompt, int completion)> onTokenUsage;
    Session::ContextBudget contextBudget;

    TodoData copyTodoData() const {
        std::lock_guard<std::mutex> lock(todoMutex);
        return todoData;
    }

    std::vector<Message> getNewMessagesSince(int64_t sinceId) const;

private:
    void reloadAllowedTools() {
        allowedTools_ = parseAvailableTools(systemPrompt_);
    }
    std::vector<ToolDefinition> getFilteredToolDefinitions() const;
    StreamCallbacks makeCallbacks();
    ChatRequest buildChatRequest() const;
    void doCompaction();
    void checkContextWarning();
    bool hasDestructiveCommand(const ToolCall& call) const;
    ToolResult executeTool(const ToolCall& call);
    void repairSession();
    void setStatus(AgentState state, const std::string& msg = "");
    void setError(const std::string& err);
    void updateSnapshot();
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

    std::string queuedUserText_;
    bool hasUserInput_ = false;

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
    int toolCallDepth_ = 0;
    int maxToolCallsPerRound_ = 30;

    std::vector<ToolCall> pendingApprovalCalls_;
    bool approvalDone_ = false;
    bool approvalGranted_ = false;
    int approvalAction_ = 0;
    std::mutex approvalMutex_;
    std::condition_variable approvalCv_;
    std::atomic<bool> destructiveApproved_{false};

    mutable size_t cachedContextTokens_ = 0;
    mutable bool contextTokensDirty_ = true;

    std::atomic<bool> cancelRequested_{false};

    bool saveRequested = false;
    bool loadRequested = false;
    TodoData todoData;
    mutable std::mutex todoMutex;

    std::string model_ = "deepseek-v4-flash";
    std::string cppCompilerPath;
    std::string pythonPath;
    std::string workspacePath_;
    std::string systemPrompt_;
    std::vector<std::string> allowedTools_;
    size_t configContextWindow = 0;
    int configMaxTokens = 65536;
    double configTemperature = 0.0;
};
