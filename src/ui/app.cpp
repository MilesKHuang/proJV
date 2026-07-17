#define IMGUI_DEFINE_MATH_OPERATORS
#include "app.h"
#include "core/prompts.h"
#include "core/config.h"
#include "tools/registry.h"
#include "tools/shell_tool.h"
#include "tools/file_tool.h"
#include "tools/md_file_tool.h"
#include "tools/web_tools.h"

#include "json.hpp"
#include "debug_log.h"
#include <windows.h>
#include <format>
#include <commdlg.h>
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
            std::filesystem::path(getConfigPath()).parent_path()
        ).string();
    }

    registerShellTool(tools, ws);
    registerFileTools(tools, ws);
    registerMdFileTool(tools, ws);
    registerEditFileTool(tools);
    // registerGitTools(tools);  // removed — git commands are covered by shell_tool
    registerFileSearchTool(tools);
    registerSearchTool(tools);
    registerFetchTool(tools);
}

bool App::initialize() {
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
    strncpy_s(baseUrlBuf, config.baseUrl.c_str(), sizeof(baseUrlBuf) - 1);
    strncpy_s(cppCompilerPathBuf, config.cppCompilerPath.c_str(), sizeof(cppCompilerPathBuf) - 1);
    strncpy_s(pythonPathBuf, config.pythonPath.c_str(), sizeof(pythonPathBuf) - 1);
    strncpy_s(workspacePathBuf, config.workspacePath.c_str(), sizeof(workspacePathBuf) - 1);
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

    // -- 准备会话目录（不创建 DB 文件，等用户发第一条消息时再创建）---
    {
        std::string exeDir = std::filesystem::path(getConfigPath()).parent_path().string();
        sessionsDir_ = exeDir + "/projv_sessions";
        std::error_code ec;
        std::filesystem::create_directories(sessionsDir_, ec);
        if (ec) {
            debugLogf("[Storage] Failed to create sessions dir '%s': %s",
                sessionsDir_.c_str(), ec.message().c_str());
        }
    }
    // 设置懒初始化回调：用户发第一条消息时创建 DB 并写入已有 system 消息
    agent->ensureStorage = [this]() {
        if (storage.isOpen()) return;
        auto now = std::time(nullptr);
        char tsBuf[64];
        tm local;
        localtime_s(&local, &now);
        strftime(tsBuf, sizeof(tsBuf), "%Y%m%d_%H%M%S", &local);
        std::string dbPath = sessionsDir_ + "/session_" + tsBuf + ".db";
        if (storage.createDatabase(dbPath)) {
            agent->setStorage(&storage);
            debugLogf("[Storage] Lazy-created database: %s", dbPath.c_str());
            // 把已在 session 中的 system 消息写入 DB
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
    if (envKey) strncpy_s(apiKeyBuf, envKey, sizeof(apiKeyBuf) - 1);
    else strncpy_s(apiKeyBuf, config.apiKey.c_str(), sizeof(apiKeyBuf) - 1);
    needsConfig = config.apiKey.empty();

    applyTheme(0);

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

void App::applyTheme(int index) {
    selectedThemeIndex = index;
    auto& style = ImGui::GetStyle();

    if (index == 0) {
        // Dark theme (VS Code-like)
        ImGui::StyleColorsDark();
        style.Colors[ImGuiCol_WindowBg] = ImVec4(0.12f, 0.12f, 0.14f, 1.00f);
        style.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.09f, 0.09f, 0.11f, 1.00f);
        style.Colors[ImGuiCol_FrameBg] = ImVec4(0.18f, 0.18f, 0.20f, 1.00f);
        style.Colors[ImGuiCol_Text] = ImVec4(0.92f, 0.92f, 0.95f, 1.00f);
        style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.50f, 0.55f, 1.00f);
        style.Colors[ImGuiCol_TitleBg] = ImVec4(0.09f, 0.09f, 0.11f, 1.00f);
        style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.09f, 0.09f, 0.11f, 0.50f);
        style.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.30f, 0.30f, 0.35f, 1.00f);
    } else {
        // GitHub Dark theme
        ImGui::StyleColorsDark();
        style.Colors[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.10f, 0.13f, 1.00f);
        style.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.07f, 0.08f, 0.11f, 1.00f);
        style.Colors[ImGuiCol_FrameBg] = ImVec4(0.13f, 0.14f, 0.17f, 1.00f);
        style.Colors[ImGuiCol_Text] = ImVec4(0.90f, 0.91f, 0.95f, 1.00f);
        style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.45f, 0.46f, 0.50f, 1.00f);
        style.Colors[ImGuiCol_TitleBg] = ImVec4(0.06f, 0.07f, 0.09f, 1.00f);
        style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.06f, 0.07f, 0.09f, 0.50f);
        style.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.25f, 0.27f, 0.31f, 1.00f);
    }
}

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

    renderConfigPopup();
    renderToolApprovalDialog();
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
            if (ImGui::MenuItem("Exit", "Alt+F4")) PostQuitMessage(0);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Settings")) {
            if (ImGui::MenuItem("Configuration")) showConfigDialog = true;
            ImGui::Separator();

            if (ImGui::BeginCombo("Theme", themeNames[selectedThemeIndex])) {
                for (int i = 0; i < 2; ++i) {
                    if (ImGui::Selectable(themeNames[i], selectedThemeIndex == i))
                        applyTheme(i);
                }
                ImGui::EndCombo();
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
            if (n >= 1000000) return std::format("{:.2f}M", n/1000000.0);
            else if (n >= 1000) return std::format("{:.2f}K", n/1000.0);
            else if (n > 0) return std::format("{}", n);
            else return std::string();
        };
        auto fmtCost = [](double v) -> std::string {
            if (v < 0.005) return std::string();
            return std::format("${:.2f}", v);
        };
        std::string pStr = fmtNum(totalPromptTokens);
        std::string coStr = fmtNum(totalCompletionTokens);
        std::string costStr = fmtCost(estimatedCost);
        bool hasData = !pStr.empty() || !coStr.empty();
        std::string tokenInfo;
        if (hasData) {
            tokenInfo = std::format(" {} | {}+{} tok{}{}",
                curModel.label,
                pStr, coStr,
                costStr.empty() ? "" : " | ~",
                costStr);
        } else {
            tokenInfo = std::format(" {}", curModel.label);
        }
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.6f, 1), "%s", tokenInfo.c_str());

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

    // Item 10.2: Status bar = model name + token consumption + tool count + context pressure
    // Detailed progress moved to chat area; operation status moved to input area.
    {
        std::string modelLabel = config.model;
        auto fmtNum = [](int n) -> std::string {
            if (n >= 1000000) return std::format("{:.2f}M", n / 1000000.0);
            if (n >= 1000)    return std::format("{:.2f}K", n / 1000.0);
            return std::format("{}", n);
        };

        // Model name
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.6f, 1), " %s", modelLabel.c_str());

        // Token consumption
        ImGui::SameLine(0, 8);
        ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.5f, 1), "tok: %s+%s",
            fmtNum(totalPromptTokens).c_str(),
            fmtNum(totalCompletionTokens).c_str());

        // Message count
        ImGui::SameLine(0, 8);
        ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.5f, 1), "msgs: %zu",
            agent ? agent->getSession().messageCount() : 0);

        // Tool count (if any tool messages exist)
        if (agent) {
            int toolCount = 0;
            for (const auto& m : agent->getSession().getContextMessages())
                if (m.role == "tool") ++toolCount;
            if (toolCount > 0) {
                ImGui::SameLine(0, 8);
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.3f, 1), "tools: %d", toolCount);
            }
        }

        // Context pressure indicator (Item 5)
        if (agent && status.state == AgentState::Thinking) {
            auto pressure = agent->getContextPressure();
            ImGui::SameLine(0, 8);
            if (pressure == Session::PressureLevel::High) {
                ImGui::TextColored(ImVec4(0.9f, 0.5f, 0.1f, 1), "ctx: HIGH");
            } else if (pressure == Session::PressureLevel::Critical) {
                ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1), "ctx: CRITICAL");
            } else if (pressure == Session::PressureLevel::Medium) {
                ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.2f, 1), "ctx: MEDIUM");
            } else {
                ImGui::TextColored(ImVec4(0.3f, 0.7f, 0.3f, 1), "ctx: LOW");
            }

            // Show estimated usage %
            size_t estimated = agent->getEstimatedContextTokens();
            size_t window = agent->getContextWindowSize();
            if (window > 0) {
                ImGui::SameLine(0, 2);
                int pct = (int)(estimated * 100 / window);
                ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.5f, 1), "(%d%%)", pct);
            }
        }

        // Thinking state indicator (compact)
        if (status.state == AgentState::ExecutingTool) {
            ImGui::SameLine(0, 8);
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.0f, 1), "running: %s (%d/%d)",
                status.currentToolName.c_str(),
                status.toolProgressCurrent,
                status.toolProgressTotal);
        } else if (status.state == AgentState::AwaitingApproval) {
            ImGui::SameLine(0, 8);
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1), "awaiting approval");
        } else if (status.state == AgentState::Error) {
            ImGui::SameLine(0, 8);
            ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1), "error: %s", status.errorMessage.c_str());
        }

        // Workspace path
        ImGui::SameLine(0, 8);
        ImGui::TextColored(ImVec4(0.35f, 0.35f, 0.40f, 1), "[ws: %s]",
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
        ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "%s", config.loadError.c_str());
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1),
        "Commands: /help, /workspace, /doctor, /clear");
    ImGui::EndChild();
}

// --- Save / Load -----------------------------------------------------------

void App::saveDialogToFile() {
    if (!agent) return;

    char filename[MAX_PATH] = {};
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = "proJV Dialog\0*.db\0All\0*.*\0";
    ofn.lpstrDefExt = "db";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameA(&ofn)) return;

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
    debugLogf("[Dialog] Saved copy to: %s", filename);
    SetWindowTextA(GetActiveWindow(),
        (std::string("proJV - Saved: ") + filename).c_str());
}

void App::loadDialogFromFile() {
    if (!agent) return;
    char filename[MAX_PATH] = {};
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = "proJV Dialog\0*.db\0All\0*.*\0";
    ofn.lpstrDefExt = "db";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
    if (!GetOpenFileNameA(&ofn)) return;

    switchToDialog(filename);
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

    // Build bubbles from messages
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
    

    auto now = std::time(nullptr);
    char buf[64];
    tm local;
    localtime_s(&local, &now);
    strftime(buf, sizeof(buf), "chat_%Y%m%d_%H%M%S.db", &local);
    std::string dbPath = sessionsDir_ + "/" + buf;

    storage.createDatabase(dbPath);
    agent->setStorage(&storage);

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

    debugLogf("[Dialog] New chat: %s", dbPath.c_str());
}

// --- TODO Panel ------------------------------------------------------------

void App::renderTodoPanel() {
    if (!agent) return;
    auto todo = agent->copyTodoData();

    {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.35f, 1.0f), "TODO");
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

                ImVec4 col(0.7f, 0.7f, 0.75f, 1.0f);
                if (line.find("[x]") != std::string::npos)
                    col = ImVec4(0.3f, 0.85f, 0.4f, 1.0f);
                else if (line.find("[*]") != std::string::npos)
                    col = ImVec4(1.0f, 0.8f, 0.3f, 1.0f);
                else if (line.find("[ ]") != std::string::npos)
                    col = ImVec4(0.5f, 0.5f, 0.6f, 1.0f);
                ImGui::TextColored(col, "%s", line.c_str());
            }
            ImGui::PopTextWrapPos();
        } else {
            ImGui::Dummy(ImVec2(0, 8.0f));
            ImGui::TextColored(ImVec4(0.35f, 0.35f, 0.40f, 1.0f), "  (No active tasks)");
        }
    }

}
