// proJV TUI -- application shell implementation (backend wiring).
#include "app_tui.h"

#include "core/prompts.h"
#include "core/config.h"
#include "core/project_context.h"
#include "tui/theme_manager.h"
#include "tools/shell_tool.h"
#include "tools/file_tool.h"
#include "tools/md_file_tool.h"
#include "tools/web_tools.h"
#include "debug_log.h"

#include <cstring>
#include <ctime>
#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;

TuiApp::TuiApp() = default;

TuiApp::~TuiApp() {
    shutdown();
}

void TuiApp::setupTools() {
    std::string ws = config.workspacePath;
    if (ws.empty()) {
        ws = fs::absolute(fs::path(getExeDir())).string();
    }

    registerShellTool(tools, ws, procRunner_);
    registerFileTools(tools, ws);
    registerMdFileTool(tools, ws);
    registerEditFileTool(tools);
    registerFileSearchTool(tools);
    registerSearchTool(tools);
    registerFetchTool(tools);

    pytoolMgr_.emplace(config.pythonPath, ws);
    pytoolMgr_->scanAndRegister(tools, procRunner_);
}

bool TuiApp::initialize(IProcessRunner* procRunner) {
    procRunner_ = procRunner;
    loadTomlConfig(config, "");
    client.setConfig(config);
    setupTools();

    agent = new Agent(client, tools);
    agent->onTokenUsage = [this](int p, int c) {
        totalPromptTokens_.fetch_add(p, std::memory_order_relaxed);
        totalCompletionTokens_.fetch_add(c, std::memory_order_relaxed);
    };
    agent->onStreamingTick = [this] {
        if (onStreamingTick) onStreamingTick();
    };
    agent->setToolPaths(config.cppCompilerPath, config.pythonPath);
    agent->setRequestParams(config.maxTokens, config.temperature);
    agent->setWorkspacePath(config.workspacePath);
    agent->setModel(config.model);
    agent->setContextWindow(config.contextWindow);

    // Prompt files from projv_files/prompts/.
    promptFiles_ = ensureDefaultPrompts();
    activePromptIndex_ = 0;
    for (size_t i = 0; i < promptFiles_.size(); ++i) {
        if (promptFiles_[i] == "coder.md") { activePromptIndex_ = static_cast<int>(i); break; }
    }
    std::string sp = loadPromptFile(promptFiles_[activePromptIndex_]);
    agent->setSystemPrompt(sp);

    // Sessions dir (lazy-create DB on first message).
    sessionsDir_ = getSessionsDir();
    std::error_code ec;
    fs::create_directories(sessionsDir_, ec);
    if (ec) {
        debugLogf("[TuiApp] Failed to create sessions dir '%s': %s",
            sessionsDir_.c_str(), ec.message().c_str());
    }

    agent->ensureStorage = [this]() {
        if (storage.isOpen()) return;
        auto now = std::time(nullptr);
        char tsBuf[64];
        tm local;
#ifdef _MSC_VER
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        strftime(tsBuf, sizeof(tsBuf), "%Y%m%d_%H%M%S", &local);
        std::string dbPath = sessionsDir_ + "/session_" + tsBuf + ".db";
        if (storage.createDatabase(dbPath)) {
            agent->setStorage(&storage);
            debugLogf("[TuiApp] Lazy-created database: %s", dbPath.c_str());
            for (const auto& msg : agent->getSession().getContextMessages()) {
                if (msg.role == "system") storage.insertMessage(msg);
            }
        } else {
            debugLogf("[TuiApp] Failed to lazy-create database: %s", dbPath.c_str());
        }
    };

    // Inject system prompt as first session message.
    agent->addPersistedMessage(Message::System(loadPromptFile(promptFiles_[activePromptIndex_])));

    // Inject project context (directory structure) so the LLM knows where
    // files actually live instead of guessing wrong paths.
    {
        std::string ctx = buildProjectContext(config.workspacePath);
        if (!ctx.empty()) {
            agent->addPersistedMessage(Message::System(ctx));
            debugLogf("[TuiApp] Injected project context (%zu chars)", ctx.size());
        }
    }

    // Theme: load from projv_files/theme/, prefer config.themeName.
    ThemeManager::instance().init(getThemeDir(), config.themeName);
    ThemeManager::instance().onThemeChanged = [this](const std::string& name) {
        config.themeName = name;
        saveConfig(config);
    };

    buildBubblesFromMessages();
    return true;
}

void TuiApp::buildBubblesFromMessages() {
    if (!agent) return;
    chatHistory.clear();
    lastMessageId_ = 0;

    auto msgs = agent->getNewMessagesSince(0);
    for (const auto& msg : msgs) {
        auto derived = bubble_model::deriveBubbles(msg);
        chatHistory.insert(chatHistory.end(), derived.begin(), derived.end());
        if (msg.id > lastMessageId_) lastMessageId_ = msg.id;
    }
}

void TuiApp::syncChatFromAgent() {
    if (!agent) return;

    auto newMsgs = agent->getNewMessagesSince(lastMessageId_);
    if (!newMsgs.empty()) {
        resetChatScroll();  // new content -> scroll back to bottom
    }
    for (const auto& msg : newMsgs) {
        auto derived = bubble_model::deriveBubbles(msg);
        chatHistory.insert(chatHistory.end(), derived.begin(), derived.end());
        if (msg.id > lastMessageId_) lastMessageId_ = msg.id;
    }

    // Cap visible bubbles at 200 to bound rendering cost.
    constexpr size_t kMaxBubbles = 200;
    if (chatHistory.size() > kMaxBubbles) {
        size_t excess = chatHistory.size() - kMaxBubbles;
        chatHistory.erase(chatHistory.begin(), chatHistory.begin() + excess);
    }
}

void TuiApp::sendMessage(const std::string& text) {
    if (!agent || text.empty()) return;
    if (agentThreadRunning_.load()) return;
    agent->startTurn(text);
    launchAgentThread();
}

void TuiApp::launchAgentThread() {
    if (agentThreadRunning_.load()) return;
    joinAgentThread();
    agentThreadRunning_.store(true);
    agentThread_ = std::thread([this]() {
        agent->newTurn();
        agent->run();
        agentThreadRunning_.store(false);
        if (onTurnComplete) onTurnComplete();
    });
}

void TuiApp::joinAgentThread() {
    if (agentThread_.joinable()) {
        try { agentThread_.join(); } catch (...) {}
    }
}

void TuiApp::shutdown() {
    if (agent) agent->cancel();
    joinAgentThread();
    delete agent;
    agent = nullptr;
}

status_bar::Data TuiApp::getStatusBarData() const {
    status_bar::Data d;
    d.model = config.model;
    d.promptTokens = totalPromptTokens_.load(std::memory_order_relaxed);
    d.completionTokens = totalCompletionTokens_.load(std::memory_order_relaxed);
    d.status = getStatus();
    d.workspace = config.workspacePath;
    if (agent) {
        d.msgCount = agent->getSession().messageCount();
        int toolCount = 0;
        for (const auto& m : agent->getSession().getContextMessages()) {
            if (m.role == "tool") ++toolCount;
        }
        d.toolCount = toolCount;
        d.pressure = agent->getContextPressure();
        d.estimatedTokens = agent->getEstimatedContextTokens();
        d.windowTokens = agent->getContextWindowSize();
    }
    return d;
}

void TuiApp::saveApiKeyAndConnect(const std::string& apiKey) {
    config.apiKey = apiKey;
    config.loaded = true;
    config.loadError.clear();
    if (!saveApiKey(config.apiKey)) {
        config.loadError = "Failed to save API key";
    }
    client.setConfig(config);
    config.loadError.clear();
}

void TuiApp::saveFullConfig(const std::string& apiKey, const std::string& baseUrl,
                            int maxTokens, double temperature, const std::string& workspace,
                            const std::string& compiler, const std::string& python) {
    config.apiKey = apiKey;
    config.baseUrl = baseUrl;
    config.maxTokens = maxTokens;
    config.temperature = temperature;
    config.workspacePath = workspace;
    config.cppCompilerPath = compiler;
    config.pythonPath = python;
    config.loaded = true;
    config.loadError.clear();

    if (!saveConfig(config)) {
        config.loadError = "Failed to save config file!";
    }

    client.setConfig(config);
    setupTools();  // re-register tools with new workspace path
    agent->setToolPaths(config.cppCompilerPath, config.pythonPath);
    agent->setRequestParams(config.maxTokens, config.temperature);
    config.loadError.clear();
}

void TuiApp::newChat() {
    if (!agent) return;
    agent->cancel();
    if (storage.isOpen()) storage.closeDatabase();
    agent->setStorage(nullptr);
    agent->clearSession();
    chatHistory.clear();
    lastMessageId_ = 0;
    totalPromptTokens_.store(0, std::memory_order_relaxed);
    totalCompletionTokens_.store(0, std::memory_order_relaxed);
    {
        std::string sp = loadPromptFile(promptFiles_[activePromptIndex_]);
        agent->setSystemPrompt(sp);
        agent->addPersistedMessage(Message::System(sp));
    }
    buildBubblesFromMessages();
}

void TuiApp::switchToDialog(const std::string& dbPath) {
    if (!agent) return;
    agent->cancel();

    std::string oldPath = storage.currentPath();
    agent->clearSession();
    chatHistory.clear();
    lastMessageId_ = 0;

    storage.closeDatabase();
    if (!storage.openDatabase(dbPath)) {
        debugLog("[TuiApp] Failed to open, reverting to previous database");
        if (!storage.openDatabase(oldPath)) {
            storage.createDatabase(sessionsDir_ + "/recovery.db");
        }
    }
    agent->setStorage(&storage);
    try {
        agent->loadFromStorage();
    } catch (const std::exception& e) {
        debugLogf("[TuiApp] loadFromStorage exception: %s", e.what());
    }
    buildBubblesFromMessages();
}

bool TuiApp::saveDialogToFile(const std::string& filename) {
    if (!agent || filename.empty()) return false;
    std::string srcPath = storage.currentPath();
    if (srcPath.empty()) return false;

    storage.closeDatabase();
    std::error_code ec;
    std::filesystem::copy_file(srcPath, filename,
        std::filesystem::copy_options::overwrite_existing, ec);
    if (!storage.openDatabase(srcPath)) {
        debugLog("[TuiApp] Reopen failed, creating recovery database");
        storage.createDatabase(srcPath);
    }
    return !ec;
}

void TuiApp::toggleLastReasoning() {
    for (auto it = chatHistory.rbegin(); it != chatHistory.rend(); ++it) {
        if (it->hasReasoning) {
            it->reasoningExpanded = !it->reasoningExpanded;
            return;
        }
    }
}

void TuiApp::scrollChat(int delta) {
    chatScroll_ += delta;
    if (chatScroll_ < 0) chatScroll_ = 0;
    int maxScroll = static_cast<int>(chatHistory.size());
    if (chatScroll_ > maxScroll) chatScroll_ = maxScroll;
}

void TuiApp::resetChatScroll() {
    chatScroll_ = 0;
}
