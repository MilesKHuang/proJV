#pragma once
#include "models.h"
#include "core/config.h"
#include "core/storage.h"
#include "client/deepseek.h"
#include "core/agent.h"
#include "tools/registry.h"
#include <imgui.h>
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <atomic>

struct ChatBubble {
    std::string role;  // "user", "assistant", "system", "tool_call", "tool_result"
    std::string content;
    std::string reasoningText;   // reasoning_content from deepseek-reasoner
    bool hasReasoning = false;   // whether this bubble has reasoning content
    bool reasoningExpanded = false; // collapsible state; default collapsed (chain-of-thought is verbose)
};

class App {
public:
    App();
    ~App();

    bool initialize();
    void render();
    bool hasApiKey() const { return config.loaded && !config.apiKey.empty(); }
    const std::string& configError() const { return config.loadError; }
    const std::string& getConfigThemeName() const { return config.themeName; }
    void setModel(const std::string& model);
    int getAgentState() const;
    
    // Dialog management
    void newChat();

    // Agent worker thread management
    void launchAgentThread();
    void joinAgentThread();
    void checkAgentThread();

private:
    AppConfig config;
    DeepSeekClient client;
    ToolRegistry tools;
    Storage storage;

    Agent* agent = nullptr;

    // UI state
    char inputBuf[16384] = {};
    std::deque<ChatBubble> chatHistory;
    std::string sessionsDir_;
    int64_t lastMessageId_ = 0;   // tracks last synced message id from SQLite
    AgentStatus lastStatus;
    bool needsConfig = false;
    char apiKeyBuf[2048] = {};
    bool scrollToBottom = false;
    bool showConfigDialog = false;
    std::vector<std::string> promptFiles_;
    int activePromptIndex_ = 0;

    // Config edit buffers
    char baseUrlBuf[512] = {};
    char cppCompilerPathBuf[512] = {};
    char pythonPathBuf[512] = {};
    char workspacePathBuf[512] = {};
    int configMaxTokens = 4096;
    float configTemperature = 0.0f;

    // Token tracking
    int totalPromptTokens = 0;
    int totalCompletionTokens = 0;

    // Available models
    std::vector<ModelInfo> availableModels;
    mutable std::mutex modelMtx;
    int selectedModelIndex = 0;
    std::atomic<bool> modelsLoading{false};



    // Render functions
    void renderMainMenuBar();
    void renderWelcomePage();
    void renderChatArea();
    void renderInputArea();
    void renderConfigPopup();
    void renderStatusBar();

    void renderToolApprovalDialog();

    // Per-frame sync: read new messages from agent, derive ChatBubbles
    void syncChatFromAgent();

    // Build all ChatBubbles from session messages (used on session load)
    void buildBubblesFromMessages();

    // Derive one or more ChatBubbles from a Message and append to chatHistory
    void deriveBubblesFromMessage(const Message& msg);

    // Session I/O
    void saveDialogToFile();
    void loadDialogFromFile();
    void switchToDialog(const std::string& dbPath);

    void setupTools();

    // Handle save/load flags from agent
    void checkAgentFlags();

    // Agent worker thread (spawned per turn)
    std::thread agentThread_;
    std::atomic<bool> agentThreadRunning_{false};
    std::thread fetchModelsThread_;

    // TODO panel
    bool showTodoPanel = true;
    void renderTodoPanel();
};