#define IMGUI_DEFINE_MATH_OPERATORS
#include "app.h"
#include "ui/theme.h"
#include "core/prompts.h"
#include "core/config.h"
#include "tools/registry.h"
#include "tools/shell_tool.h"
#include "tools/file_tool.h"
#include "tools/md_file_tool.h"
#include "tools/web_tools.h"

#include "json.hpp"
#include "debug_log.h"
#include "platform_compat.h"
#include "platform/isystem_util.h"
#include <GLFW/glfw3.h>
#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#endif
#ifdef _MSC_VER
#include <format>
#define std_format std::format
#else
#include <cstdio>
#include "format_compat.h"
#define std_format projv::fmt
#endif
#include <imgui.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>
#include <cstring>

// --- Project context prefix/suffix (moved from prompts.h) ------------
static constexpr const char* PROJECT_CONTEXT_PREFIX =
    "[PROJECT CONTEXT] Workspace directory structure -- use these paths with read_file:\n"
    "Workspace: ";
static constexpr const char* PROJECT_CONTEXT_SUFFIX = "\n[END PROJECT CONTEXT]";

// --- Project context builder ------------------------------------------
// Scans the workspace directory and builds a compact directory-structure
// overview so the LLM knows where files actually live instead of guessing
// wrong paths.
static std::string buildProjectContext(const std::string& workspacePath) {
    namespace fs = std::filesystem;
    std::error_code ec;

    // Determine the root to scan
    fs::path root;
    if (workspacePath.empty()) {
        root = fs::current_path(ec);
        if (ec) return "";
    } else {
        root = fs::absolute(workspacePath, ec);
        if (ec) return "";
    }
    if (!fs::is_directory(root, ec) || ec) return "";

    // Directories to skip (common build/output/vendor dirs)
    static const std::vector<std::string> skipDirs = {
        ".git", ".vs", ".vscode", ".idea",
        "build", "Debug", "Release", "x64", "x86",
        "bin", "bin64", "obj", "out", "target",
        "node_modules", "packages", "vendor", "external",
        "__pycache__", ".pytest_cache", ".mypy_cache",
        "nix", "deploy", "integrations", "fuzzing",
        "docs", "examples", "tests", "test", ".github"
    };

    static const std::vector<std::string> srcExts = {
        ".cpp", ".c", ".h", ".hpp", ".rs", ".py", ".go", ".ts", ".js",
        ".toml", ".json", ".yaml", ".yml", ".cmake", ".txt", ".md",
        "CMakeLists.txt", "Makefile", "Cargo.toml", "package.json"
    };

    auto shouldSkip = [](const std::string& name) -> bool {
        if (name.empty() || name[0] == '.') return true;
        for (const auto& d : skipDirs) {
            if (name == d) return true;
        }
        return false;
    };

    auto isSrcFile = [](const std::string& name) -> bool {
        for (const auto& ext : srcExts) {
            if (name.size() >= ext.size() &&
                name.compare(name.size() - ext.size(), ext.size(), ext) == 0)
                return true;
        }
        // Also match exact names (CMakeLists.txt etc handled above)
        return false;
    };

    std::ostringstream out;
    out << PROJECT_CONTEXT_PREFIX << root.string() << "\n\n";

    // -- Scan top-level entries -----------------------------------------
    std::vector<fs::directory_entry> topEntries;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        topEntries.push_back(entry);
        if (ec) { ec.clear(); continue; }
    }
    // Sort: directories first, then alphabetical
    std::sort(topEntries.begin(), topEntries.end(),
        [](const fs::directory_entry& a, const fs::directory_entry& b) {
            bool aDir = a.is_directory();
            bool bDir = b.is_directory();
            if (aDir != bDir) return aDir > bDir;
            return a.path().filename().string() < b.path().filename().string();
        });

    size_t totalChars = out.str().size();
    const size_t MAX_CHARS = 4500;

    int dirsListed = 0;
    for (const auto& entry : topEntries) {
        if (totalChars > MAX_CHARS) break;

        auto name = entry.path().filename().string();
        if (shouldSkip(name)) continue;

        if (entry.is_directory(ec)) {
            if (ec) { ec.clear(); continue; }
            ++dirsListed;
            out << "  " << name << "/\n";

            // -- One level deeper: list subdirectories and source files --
            int subCount = 0;
            for (const auto& sub : fs::directory_iterator(entry.path(), ec)) {
                if (ec) { ec.clear(); break; }
                if (totalChars > MAX_CHARS || subCount > 40) break;

                auto subName = sub.path().filename().string();
                if (shouldSkip(subName)) continue;

                if (sub.is_directory(ec)) {
                    if (ec) { ec.clear(); continue; }
                    out << "    " << subName << "/\n";
                    ++subCount;
                    totalChars = out.str().size();

                    // -- Two levels deeper: list source files only ------
                    int fileCount = 0;
                    for (const auto& f : fs::directory_iterator(sub.path(), ec)) {
                        if (ec) { ec.clear(); break; }
                        if (totalChars > MAX_CHARS || fileCount > 30) break;

                        auto fName = f.path().filename().string();
                        if (f.is_regular_file(ec) && isSrcFile(fName)) {
                            if (ec) { ec.clear(); continue; }
                            out << "      " << fName << "\n";
                            ++fileCount;
                            totalChars = out.str().size();
                        }
                    }
                } else if (sub.is_regular_file(ec) && isSrcFile(subName)) {
                    if (ec) { ec.clear(); continue; }
                    out << "    " << subName << "\n";
                    ++subCount;
                    totalChars = out.str().size();
                }
            }
        } else if (entry.is_regular_file(ec) && isSrcFile(name)) {
            if (ec) { ec.clear(); continue; }
            out << "  " << name << "\n";
            totalChars = out.str().size();
        }
    }

    if (totalChars > MAX_CHARS) {
        out << "  ... (truncated, " << dirsListed << " dirs listed)\n";
    }

    out << PROJECT_CONTEXT_SUFFIX;
    return out.str();
}

App::App() {}
App::~App() {
    // 1. Cancel agent and join worker thread
    if (agent) {
        agent->cancel();
        joinAgentThread();
    }
    // 2. Join fetchModels thread if still running
    if (fetchModelsThread_.joinable()) fetchModelsThread_.join();
    // 3. Delete agent
    delete agent;
    // 4. Storage destructor closes both connections naturally
}

void App::setupTools() {
    std::string ws = config.workspacePath;
    if (ws.empty()) {
        ws = std::filesystem::absolute(
            std::filesystem::path(getExeDir())
        ).string();
    }

    registerShellTool(tools, ws, procRunner_);
    registerFileTools(tools, ws);
    registerMdFileTool(tools, ws);
    registerEditFileTool(tools);
    // registerGitTools(tools);  // removed -- git commands are covered by shell_tool
    registerFileSearchTool(tools);
    registerSearchTool(tools);
    registerFetchTool(tools);

    // Python tools (auto-discovered from projv_pytool/)
    pytoolMgr_.emplace(config.pythonPath, ws);
    pytoolMgr_->scanAndRegister(tools, procRunner_);
}

bool App::initialize(IProcessRunner* procRunner) {
    procRunner_ = procRunner;
    loadTomlConfig(config, "");
    client.setConfig(config);
    setupTools();

    agent = new Agent(client, tools);

    // Token usage callback
    agent->onTokenUsage = [this](int p, int c) {
        totalPromptTokens += p;
        totalCompletionTokens += c;
    };
    agent->setToolPaths(config.cppCompilerPath, config.pythonPath);
    agent->setRequestParams(config.maxTokens, config.temperature);
    agent->setWorkspacePath(config.workspacePath);
    agent->setModel(config.model);
    agent->setContextWindow(config.contextWindow);

    // -- Initialize config edit buffers from loaded config ----------
    strncpy(baseUrlBuf, config.baseUrl.c_str(), sizeof(baseUrlBuf) - 1);
    strncpy(cppCompilerPathBuf, config.cppCompilerPath.c_str(), sizeof(cppCompilerPathBuf) - 1);
    strncpy(pythonPathBuf, config.pythonPath.c_str(), sizeof(pythonPathBuf) - 1);
    strncpy(workspacePathBuf, config.workspacePath.c_str(), sizeof(workspacePathBuf) - 1);
    configMaxTokens = config.maxTokens;
    configTemperature = static_cast<float>(config.temperature);

    // -- Initialize prompt files from projv_prompts/ directory
    {
        promptFiles_ = ensureDefaultPrompts();
        activePromptIndex_ = 0;
        // Find coder.md in the list for initial prompt
        for (int i = 0; i < (int)promptFiles_.size(); ++i) {
            if (promptFiles_[i] == "coder.md") { activePromptIndex_ = i; break; }
        }
    }

    // -- Load initial prompt and set allowed tools whitelist
    {
        std::string sp = loadPromptFile(promptFiles_[activePromptIndex_]);
        if (agent) agent->setSystemPrompt(sp);
    }

    // -- Prepare sessions dir (lazy-create DB on first message) ---
    {
        sessionsDir_ = getSessionsDir();
        std::error_code ec;
        std::filesystem::create_directories(sessionsDir_, ec);
        if (ec) {
            debugLogf("[Storage] Failed to create sessions dir '%s': %s",
                sessionsDir_.c_str(), ec.message().c_str());
        }
    }
    // Lazy-init: create DB + write existing system messages
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
            debugLogf("[Storage] Lazy-created database: %s", dbPath.c_str());
            // Write existing session system msgs to DB
            for (const auto& msg : agent->getSession().getContextMessages()) {
                if (msg.role == "system") {
                    storage.insertMessage(msg);
                }
            }
        } else {
            debugLogf("[Storage] Failed to lazy-create database: %s", dbPath.c_str());
        }
    };

    // -- Inject system prompt as first session message --------------
    agent->addPersistedMessage(Message::System(loadPromptFile(promptFiles_[activePromptIndex_])));

    // -- Inject project context (directory structure) ----------------
    // This tells the LLM where files actually live so it doesn't guess
    // wrong paths (e.g. models.h is in src/client/, NOT src/core/).
    {
        std::string ctx = buildProjectContext(config.workspacePath);
        if (!ctx.empty()) {
            agent->addPersistedMessage(Message::System(ctx));
            debugLogf("[App] Injected project context (%zu chars)", ctx.size());
        }
    }

    // -- Build initial chat bubbles from messages -------------------
    buildBubblesFromMessages();

    const char* envKey = std::getenv("DEEPSEEK_API_KEY");
    if (envKey) strncpy(apiKeyBuf, envKey, sizeof(apiKeyBuf) - 1);
    else strncpy(apiKeyBuf, config.apiKey.c_str(), sizeof(apiKeyBuf) - 1);
    needsConfig = config.apiKey.empty();

    // Theme is initialized in main.cpp via ThemeManager::init() after this function returns.
    // Wire up the theme-changed callback to persist preference to config.toml
    ThemeManager::instance().onThemeChanged = [this](const std::string& name) {
        config.themeName = name;
        saveConfig(config);
    };

    // -- R1/D: fetchModels managed thread, UI uses default model placeholder
    if (!config.apiKey.empty()) {
        modelsLoading.store(true, std::memory_order_release);
        if (fetchModelsThread_.joinable()) fetchModelsThread_.join();
        fetchModelsThread_ = std::thread([this]() {
            std::string err;
            auto models = client.fetchModels(&err);
            {
                std::lock_guard<std::mutex> lock(modelMtx);
                if (!models.empty()) {
                    availableModels = std::move(models);
                    selectedModelIndex = 0;
                    for (size_t i = 0; i < availableModels.size(); ++i) {
                        if (availableModels[i].id == config.model) {
                            selectedModelIndex = (int)i;
                            break;
                        }
                    }
                }
                modelsLoading.store(false, std::memory_order_release);
            }
        });
    }

    return true;
}

void App::setModel(const std::string& model) {
    config.model = model;
}

int App::getAgentState() const {
    if (!agent) return -1;
    auto st = agent->getStatus();
    return (int)st.state;
}



// Forward declarations for pickers (defined later)
#ifndef _WIN32
static std::string pickSavePath();
static std::string pickOpenPath();
#endif

// --- Agent flag checks -----------------------------------------------------
void App::checkAgentFlags() {
    if (!agent) return;
    if (agent->isSaveRequested()) {
        agent->clearSaveRequested();
        saveDialogToFile();
    }
    if (agent->isLoadRequested()) {
        agent->clearLoadRequested();
        loadDialogFromFile();
    }
}

// --- Render ----------------------------------------------------------------

void App::render() {
#ifndef _WIN32
    // Deferred file dialogs: run BEFORE ImGui frame to avoid corrupting menu state
    if (pendingOpenChat_) {
        pendingOpenChat_ = false;
        std::string filename = pickOpenPath();
        if (!filename.empty()) switchToDialog(filename);
    }
    if (pendingSaveChat_) {
        pendingSaveChat_ = false;
        std::string filename = pickSavePath();
        if (!filename.empty()) {
            std::string srcPath = storage.currentPath();
            if (!srcPath.empty()) {
                storage.closeDatabase();
                std::filesystem::copy_file(srcPath, filename,
                    std::filesystem::copy_options::overwrite_existing);
                if (!storage.openDatabase(srcPath)) {
                    debugLog("[Dialog] Reopen failed, creating recovery database");
                    storage.createDatabase(srcPath);
                }
                debugLogf("[Dialog] Saved copy to: %s", filename.c_str());
            }
        }
    }
#endif

    // -- Per-frame diagnostic: log agent state every ~60 frames when active --
    static int frameSkip = 0;
    static AgentState lastLoggedState = AgentState::Idle;
    if (agent) {
        auto st = agent->getStatus();
        if (st.state != AgentState::Idle || lastLoggedState != AgentState::Idle) {
            if (++frameSkip >= 60 || st.state != lastLoggedState) {
                frameSkip = 0;
                lastLoggedState = st.state;
            }
        }
    }

    // Heartbeat: log a few frames after returning to Idle to catch silent stalls
    static int idleHeartbeat = 0;
    static AgentState lastHeartbeatState = AgentState::Idle;
    bool shouldLogThisFrame = false;
    if (agent) {
        auto st = agent->getStatus();
        if (st.state != AgentState::Idle) {
            idleHeartbeat = 0;
            // Throttle: log at most once per second during busy
            static auto lastBusyLogTime = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            shouldLogThisFrame = (now - lastBusyLogTime >= std::chrono::seconds(1));
            if (shouldLogThisFrame) lastBusyLogTime = now;
        } else if (idleHeartbeat < 5) {
            ++idleHeartbeat;
            shouldLogThisFrame = true;
        }
        lastHeartbeatState = st.state;
    }

    syncChatFromAgent();
    checkAgentThread();
    checkAgentFlags();

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar;

    ImGui::Begin("proJV", nullptr, flags);

    // When agent is busy, disable all UI except Cancel button
    bool isWaiting = false;
    if (agent) {
        auto st = agent->getStatus();
        isWaiting = (st.state != AgentState::Idle && st.state != AgentState::Error);
    }
    if (isWaiting) ImGui::BeginDisabled();

    renderMainMenuBar();

    if (needsConfig || !hasApiKey()) {
        renderWelcomePage();
    } else {
        float footerHeight = std::max(viewport->Size.y / 8.0f, ImGui::GetFrameHeightWithSpacing() * 4.0f);
        ImGui::BeginChild("MainArea", ImVec2(0, -footerHeight), false);

        if (showTodoPanel) {
            ImGui::BeginChild("ChatPanel", ImVec2(-280.0f, 0), false);
            renderChatArea();
            ImGui::EndChild();
            ImGui::SameLine();
            ImGui::BeginChild("TodoPanel", ImVec2(280.0f, 0), true,
                ImGuiWindowFlags_AlwaysVerticalScrollbar);
            renderTodoPanel();
            ImGui::EndChild();
        } else {
            renderChatArea();
        }

        ImGui::EndChild();

        renderInputArea();
    }

    renderStatusBar();

    if (isWaiting) ImGui::EndDisabled();

    ImGui::End();

    if (showConfigDialog) {
        ImGui::OpenPopup("Configuration");
        showConfigDialog = false;
    }

    if (agent) {
        auto agentStatus = agent->getStatus();
        if (agentStatus.state == AgentState::AwaitingApproval) {
            bool alreadyOpen = ImGui::IsPopupOpen("Delete Confirmation");
            if (!alreadyOpen) {
                ImGui::OpenPopup("Delete Confirmation");
            }
        } else {
            // If popup is still open but agent left AwaitingApproval, log the transition
            static bool popupWasOpen = false;
            bool isOpen = ImGui::IsPopupOpen("Delete Confirmation");
            if (isOpen && !popupWasOpen) {
                popupWasOpen = true;
            } else if (!isOpen && popupWasOpen) {
                popupWasOpen = false;
            }
        }
    }

    if (showThemeEditor) {
        ImGui::OpenPopup("Theme Editor");
        showThemeEditor = false;
    }
    renderConfigPopup();
    renderToolApprovalDialog();
    renderThemePopup();
}

void App::renderMainMenuBar() {
    static int mBarFrameSkip = 0;
    bool mBarLog = (++mBarFrameSkip >= 50);  // log ~1/sec
    if (mBarLog) mBarFrameSkip = 0;

    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New Chat", "Ctrl+N")) {
                newChat();
            }
            if (ImGui::MenuItem("Save As...", "Ctrl+S")) saveDialogToFile();
            if (ImGui::MenuItem("Open Chat...", "Ctrl+O")) loadDialogFromFile();
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Alt+F4"))
                glfwSetWindowShouldClose(glfwGetCurrentContext(), GLFW_TRUE);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Settings")) {
            if (ImGui::MenuItem("Configuration")) showConfigDialog = true;
            ImGui::Separator();

            {
                // Dynamic theme combo: builtins + installed + Customize...
                const auto& curTheme = ThemeManager::instance().current();
                std::string curName = curTheme.name.empty() ? "Obsidian" : curTheme.name;
                if (ImGui::BeginCombo("Theme", curName.c_str())) {
                    // Builtin themes
                    if (ImGui::Selectable("Obsidian", curName == "Obsidian"))
                        ThemeManager::instance().switchTo("Obsidian");
                    if (ImGui::Selectable("Light", curName == "Light"))
                        ThemeManager::instance().switchTo("Light");

                    // Installed themes
                    const auto& installed = ThemeManager::instance().installedNames();
                    if (!installed.empty()) {
                        ImGui::Separator();
                        for (const auto& name : installed) {
                            if (ImGui::Selectable(name.c_str(), curName == name))
                                ThemeManager::instance().switchTo(name);
                        }
                    }

                    ImGui::Separator();
                    if (ImGui::Selectable("Customize...")) {
                        showThemeEditor = true;
                    }
                    ImGui::EndCombo();
                }
            }

            int curSel;
            std::vector<ModelInfo> modelsSnap;
            {
                std::lock_guard<std::mutex> lock(modelMtx);
                curSel = selectedModelIndex;
                if (availableModels.empty() && !modelsLoading) {
                    ModelInfo chat, reason;
                    chat.id = "deepseek-chat"; chat.label = "deepseek-chat";
                    chat.inputPricePerM = lookupModelPrice(config.modelPrices, "deepseek-chat", true);
                    chat.outputPricePerM = lookupModelPrice(config.modelPrices, "deepseek-chat", false);
                    chat.supportsTools = modelSupportsTools("deepseek-chat");
                    reason.id = "deepseek-reasoner"; reason.label = "deepseek-reasoner";
                    reason.inputPricePerM = lookupModelPrice(config.modelPrices, "deepseek-reasoner", true);
                    reason.outputPricePerM = lookupModelPrice(config.modelPrices, "deepseek-reasoner", false);
                    reason.supportsTools = modelSupportsTools("deepseek-reasoner");
                    availableModels.push_back(chat);
                    availableModels.push_back(reason);
                    selectedModelIndex = (config.model == reason.id) ? 1 : 0;
                    curSel = selectedModelIndex;
                }
                modelsSnap = availableModels;
                curSel = selectedModelIndex;
            }
            if (!modelsSnap.empty()) {
                const char* comboLabel = modelsLoading ? "Loading models..." : modelsSnap[curSel].label.c_str();
                if (ImGui::BeginCombo("Model", comboLabel)) {
                    for (int i = 0; i < (int)modelsSnap.size(); ++i) {
                        std::string label = modelsSnap[i].label;
                        if (!modelsSnap[i].supportsTools)
                            label += " (no tools)";
                        if (ImGui::Selectable(label.c_str(), i == curSel)) {
                            std::lock_guard<std::mutex> lock(modelMtx);
                            selectedModelIndex = i;
                            config.model = modelsSnap[i].id;
                            if (agent) {
                                agent->setModel(config.model);
                                agent->setContextWindow(0);  // reset auto-detect on model switch
                            }
                        }
                    }
                    ImGui::EndCombo();
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("TODO Panel", "Ctrl+T", &showTodoPanel);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("About proJV")) ImGui::OpenPopup("About");
            ImGui::EndMenu();
        }

        // Right side: token info + cost
        ImGui::SameLine(ImGui::GetWindowWidth() - 380);
        ModelInfo curModel;
        {
            std::lock_guard<std::mutex> lock(modelMtx);
            if (!availableModels.empty())
                curModel = availableModels[selectedModelIndex];
            else
                curModel = {"deepseek-chat", "deepseek-chat", 0.5, 2.0, true};
        }
        double inputPrice = curModel.inputPricePerM * 1e-6;
        double outputPrice = curModel.outputPricePerM * 1e-6;
        double estimatedCost = (double)totalPromptTokens * inputPrice +
                               (double)totalCompletionTokens * outputPrice;
        auto fmtNum = [](int n) -> std::string {
            char b[32];
            if (n >= 1000000) { snprintf(b, 32, "%.2fM", n/1000000.0); return b; }
            else if (n >= 1000) { snprintf(b, 32, "%.2fK", n/1000.0); return b; }
            else if (n > 0) { snprintf(b, 32, "%d", n); return b; }
            return {};
        };
        auto fmtCost = [](double v) -> std::string {
            if (v < 0.005) return {};
            char b[32]; snprintf(b, 32, "$%.2f", v); return b;
        };
        std::string pStr = fmtNum(totalPromptTokens);
        std::string coStr = fmtNum(totalCompletionTokens);
        std::string costStr = fmtCost(estimatedCost);
        bool hasData = !pStr.empty() || !coStr.empty();
        std::string tokenInfo;
        if (hasData) {
            char tb[256];
            snprintf(tb, sizeof(tb), " %s | %s+%s tok%s%s",
                curModel.label.c_str(),
                pStr.c_str(), coStr.c_str(),
                costStr.empty() ? "" : " | ~",
                costStr.c_str());
            tokenInfo = tb;
        } else {
            tokenInfo = " " + curModel.label;
        }
        ImGui::TextColored(ThemeColors::toVec4(ThemeManager::instance().current().statusTokenInfo), "%s", tokenInfo.c_str());

        ImGui::EndMenuBar();
    }

    if (ImGui::BeginPopupModal("About", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("proJV v0.1.0");
        ImGui::Text("Minimal DeepSeek AI Agent (imgui/C++)");
        ImGui::Separator();
        if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// --- Status bar ------------------------------------------------------------

void App::renderStatusBar() {
    ImGui::Separator();
    auto status = agent ? agent->getStatus() : AgentStatus{};
    const auto& T = ThemeManager::instance().current();

    {
        std::string modelLabel = config.model;
        auto fmtNum = [](int n) -> std::string {
            char b[32];
            if (n >= 1000000) { snprintf(b, 32, "%.2fM", n/1000000.0); return b; }
            if (n >= 1000)    { snprintf(b, 32, "%.2fK", n/1000.0); return b; }
            snprintf(b, 32, "%d", n); return b;
        };

        // Model name
        ImGui::TextColored(ThemeColors::toVec4(T.statusModelName), " %s", modelLabel.c_str());

        // Token consumption
        ImGui::SameLine(0, 8);
        ImGui::TextColored(ThemeColors::toVec4(T.statusTokenCount), "tok: %s+%s",
            fmtNum(totalPromptTokens).c_str(),
            fmtNum(totalCompletionTokens).c_str());

        // Message count
        ImGui::SameLine(0, 8);
        ImGui::TextColored(ThemeColors::toVec4(T.statusMsgCount), "msgs: %zu",
            agent ? agent->getSession().messageCount() : 0);

        // Tool count (if any tool messages exist)
        if (agent) {
            int toolCount = 0;
            for (const auto& m : agent->getSession().getContextMessages())
                if (m.role == "tool") ++toolCount;
            if (toolCount > 0) {
                ImGui::SameLine(0, 8);
                ImGui::TextColored(ThemeColors::toVec4(T.statusToolCount), "tools: %d", toolCount);
            }
        }

        // Context pressure indicator (Item 5)
        if (agent && status.state == AgentState::Thinking) {
            auto pressure = agent->getContextPressure();
            ImGui::SameLine(0, 8);
            if (pressure == Session::PressureLevel::High) {
                ImGui::TextColored(ThemeColors::toVec4(T.statusCtxHigh), "ctx: HIGH");
            } else if (pressure == Session::PressureLevel::Critical) {
                ImGui::TextColored(ThemeColors::toVec4(T.statusCtxCritical), "ctx: CRITICAL");
            } else if (pressure == Session::PressureLevel::Medium) {
                ImGui::TextColored(ThemeColors::toVec4(T.statusCtxMedium), "ctx: MEDIUM");
            } else {
                ImGui::TextColored(ThemeColors::toVec4(T.statusCtxLow), "ctx: LOW");
            }

            // Show estimated usage %
            size_t estimated = agent->getEstimatedContextTokens();
            size_t window = agent->getContextWindowSize();
            if (window > 0) {
                ImGui::SameLine(0, 2);
                int pct = (int)(estimated * 100 / window);
                ImGui::TextColored(ThemeColors::toVec4(T.statusCtxPercent), "(%d%%)", pct);
            }
        }

        // Thinking state indicator (compact)
        if (status.state == AgentState::ExecutingTool) {
            ImGui::SameLine(0, 8);
            ImGui::TextColored(ThemeColors::toVec4(T.statusRunning), "running: %s (%d/%d)",
                status.currentToolName.c_str(),
                status.toolProgressCurrent,
                status.toolProgressTotal);
        } else if (status.state == AgentState::AwaitingApproval) {
            ImGui::SameLine(0, 8);
            ImGui::TextColored(ThemeColors::toVec4(T.statusAwaiting), "awaiting approval");
        } else if (status.state == AgentState::Error) {
            ImGui::SameLine(0, 8);
            ImGui::TextColored(ThemeColors::toVec4(T.statusError), "error: %s", status.errorMessage.c_str());
        }

        // Workspace path
        ImGui::SameLine(0, 8);
        ImGui::TextColored(ThemeColors::toVec4(T.statusWorkspace), "[ws: %s]",
            config.workspacePath.empty() ? "exe dir" : config.workspacePath.c_str());
    }
}

// --- Welcome page ----------------------------------------------------------

void App::renderWelcomePage() {
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float w = avail.x * 0.5f;
    float h = 300;
    ImGui::SetCursorPos(ImVec2((avail.x - w) * 0.5f, (avail.y - h) * 0.5f));
    ImGui::BeginChild("Welcome", ImVec2(w, h), true);

    ImGui::Text("Welcome to proJV");
    ImGui::Separator();
    ImGui::TextWrapped("You need a DeepSeek API key to get started.");
    ImGui::Spacing();
    ImGui::Text("API Key:");
    ImGui::SameLine();
    ImGui::InputText("##welcome_apikey", apiKeyBuf, sizeof(apiKeyBuf),
        ImGuiInputTextFlags_Password);
    ImGui::Spacing();

    if (ImGui::Button("Save && Connect", ImVec2(180, 32))) {
        config.apiKey = apiKeyBuf;
        config.loaded = true;
        config.loadError.clear();
        if (!saveApiKey(config.apiKey)) config.loadError = "Failed to save config file!";
        client.setConfig(config);
        needsConfig = false;
        config.loadError.clear();
    }

    if (!config.loadError.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ThemeColors::toVec4(ThemeManager::instance().current().welcomeError), "%s", config.loadError.c_str());
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(ThemeColors::toVec4(ThemeManager::instance().current().welcomeHelp),
        "Commands: /help, /workspace, /doctor, /clear");
    ImGui::EndChild();
}

// --- Save / Load -----------------------------------------------------------
// Platform-specific path pickers (only the dialog mechanism differs)

static std::string pickSavePath() {
#ifdef _WIN32
    char filename[MAX_PATH] = {};
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = "proJV Dialog\0*.db\0All\0*.*\0";
    ofn.lpstrDefExt = "db";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
    if (GetSaveFileNameA(&ofn)) return filename;
    return "";
#else
    std::string cmd =
        "zenity --file-selection --save --confirm-overwrite "
        "--file-filter='*.db' --title='Save Chat As' 2>/dev/null";
    FILE* f = popen(cmd.c_str(), "r");
    if (!f) return "";
    char buf[1024];
    std::string filename;
    if (fgets(buf, sizeof(buf), f)) {
        filename = buf;
        while (!filename.empty() && (filename.back() == '\n' || filename.back() == '\r'))
            filename.pop_back();
    }
    pclose(f);
    return filename;
#endif
}

static std::string pickOpenPath() {
#ifdef _WIN32
    char filename[MAX_PATH] = {};
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = "proJV Dialog\0*.db\0All\0*.*\0";
    ofn.lpstrDefExt = "db";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
    if (GetOpenFileNameA(&ofn)) return filename;
    return "";
#else
    std::string cmd =
        "zenity --file-selection --file-filter='*.db' "
        "--title='Open Chat' 2>/dev/null";
    FILE* f = popen(cmd.c_str(), "r");
    if (!f) return "";
    char buf[1024];
    std::string filename;
    if (fgets(buf, sizeof(buf), f)) {
        filename = buf;
        while (!filename.empty() && (filename.back() == '\n' || filename.back() == '\r'))
            filename.pop_back();
    }
    pclose(f);
    return filename;
#endif
}

void App::saveDialogToFile() {
    if (!agent) return;
#ifdef _WIN32
    std::string filename = pickSavePath();
    if (filename.empty()) return;

    std::string srcPath = storage.currentPath();
    if (srcPath.empty()) {
        debugLog("[Dialog] saveDialogToFile: no open database");
        return;
    }
    
    storage.closeDatabase();
    std::filesystem::copy_file(srcPath, filename,
        std::filesystem::copy_options::overwrite_existing);
    if (!storage.openDatabase(srcPath)) {
        debugLog("[Dialog] Reopen failed, creating recovery database");
        storage.createDatabase(srcPath);
    }
    debugLogf("[Dialog] Saved copy to: %s", filename.c_str());
    SetWindowTextA(GetActiveWindow(),
        (std::string("proJV - Saved: ") + filename).c_str());
#else
    pendingSaveChat_ = true;   // deferred: handled before next ImGui frame
#endif
}

void App::loadDialogFromFile() {
    if (!agent) return;
#ifdef _WIN32
    std::string filename = pickOpenPath();
    if (filename.empty()) return;
    switchToDialog(filename);
#else
    pendingOpenChat_ = true;   // deferred: handled before next ImGui frame
#endif
}
void App::switchToDialog(const std::string& dbPath) {
    if (!agent) return;
    agent->cancel();

    std::string oldPath = storage.currentPath();
    agent->clearSession();
    chatHistory.clear();
    lastMessageId_ = 0;

    storage.closeDatabase();
    if (!storage.openDatabase(dbPath)) {
        debugLog("[Dialog] Failed to open, reverting to previous database");
        if (!storage.openDatabase(oldPath))
            storage.createDatabase(sessionsDir_ + "/recovery.db");
    }
    agent->setStorage(&storage);
    try {
        agent->loadFromStorage();
    } catch (const std::exception& e) {
        debugLogf("[Dialog] loadFromStorage exception: %s", e.what());
    }

    buildBubblesFromMessages();
    debugLogf("[Dialog] Switched to: %s (%zu bubbles, %zu messages)",
        dbPath.c_str(), chatHistory.size(), agent->getSession().messageCount());
}

// ---- Agent thread management --------------------------------------------
void App::launchAgentThread() {
    if (agentThreadRunning_.load()) return;
    joinAgentThread();
    agentThreadRunning_.store(true);
    agentThread_ = std::thread([this]() {
        agent->newTurn();
        agent->run();
        agentThreadRunning_.store(false);
    });
}

void App::joinAgentThread() {
    if (agentThread_.joinable()) {
        try { agentThread_.join(); } catch (...) {}
    }
}

void App::checkAgentThread() {
    if (!agentThreadRunning_.load() && agentThread_.joinable()) {
        joinAgentThread();
    }
}

// --- New Chat -------------------------------------------------------------

void App::newChat() {

    if (!agent) return;

    agent->cancel();
    
    // Close current DB; new DB is created lazily on first message via ensureStorage
    if (storage.isOpen())
        storage.closeDatabase();
    agent->setStorage(nullptr);

    agent->clearSession();
    chatHistory.clear();
    lastMessageId_ = 0;
    totalPromptTokens = totalCompletionTokens = 0;

    {
        std::string sp = loadPromptFile(promptFiles_[activePromptIndex_]);
        agent->setSystemPrompt(sp);
        agent->addPersistedMessage(Message::System(sp));
    }

    // Build bubbles from messages
    buildBubblesFromMessages();

    debugLog("[Dialog] New chat (db lazy)");
}

// --- TODO Panel ------------------------------------------------------------

void App::renderTodoPanel() {
    if (!agent) return;
    auto todo = agent->copyTodoData();
    const auto& T = ThemeManager::instance().current();

    {
        ImGui::TextColored(ThemeColors::toVec4(T.todoTitle), "TODO");
        ImGui::Separator();

        if (!todo.pendingTodo.empty()) {
            ImGui::PushTextWrapPos(0.0f);
            std::string text = todo.pendingTodo;
            size_t pos = 0;
            int lineCount = 0;
            while (pos < text.size() && lineCount < 100) {
                size_t nl = text.find('\n', pos);
                std::string line = (nl == std::string::npos)
                    ? text.substr(pos)
                    : text.substr(pos, nl - pos);
                pos = (nl == std::string::npos) ? text.size() : nl + 1;
                ++lineCount;

                ImVec4 col = ThemeColors::toVec4(T.todoPending);
                if (line.find("[x]") != std::string::npos)
                    col = ThemeColors::toVec4(T.todoDone);
                else if (line.find("[*]") != std::string::npos)
                    col = ThemeColors::toVec4(T.todoInProgress);
                else if (line.find("[ ]") != std::string::npos)
                    col = ThemeColors::toVec4(T.todoOpen);
                ImGui::TextColored(col, "%s", line.c_str());
            }
            ImGui::PopTextWrapPos();
        } else {
            ImGui::Dummy(ImVec2(0, 8.0f));
            ImGui::TextColored(ThemeColors::toVec4(T.todoEmpty), "  (No active tasks)");
        }
    }

}
