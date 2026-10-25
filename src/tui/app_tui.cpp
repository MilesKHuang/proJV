// proJV TUI -- application shell implementation (backend wiring).
#include "app_tui.h"

#include "core/prompts.h"
#include "core/config.h"
#include "core/project_context.h"
#include "core/image_input.h"
#include "core/skill_trigger.h"
#include "tui/theme_manager.h"
#include "tools/shell_tool.h"
#include "tools/file_tool.h"
#include "tools/format_tool.h"
#include "tools/md_file_tool.h"
#include "tools/web_tools.h"
#include "tools/json_utils.h"
#include "debug_log.h"

#include <algorithm>
#include <cctype>
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
    registerFormatTool(tools, ws, procRunner_, config.clangFormatPath);
    registerMdFileTool(tools, ws);
    registerEditFileTool(tools, ws);
    registerFileSearchTool(tools, ws);
    registerSearchTool(tools, client);
    registerFetchTool(tools, client);

    // run_skill: start an installed skill. The skill does NOT run inside this
    // executor -- it runs as its own phase right after the current turn ends
    // (see TuiApp::runAgentTurnLoop), through the very same function the
    // "/<skill> <topic>" trigger uses. Consequences:
    //   - narration/document are normal assistant messages: bubbles, the agent
    //     dock, the DB and session replay all behave like a user-triggered run;
    //   - the tool_call pairing is never broken, because this executor returns a
    //     plain tool result and the turn stops right after it;
    //   - ESC keeps working (cancelTurn cancels the published SkillRunner).
    {
        ToolDefinition def;
        def.name = "run_skill";
        def.description =
            "Start an installed multi-agent skill (a fixed multi-role workflow). The skill "
            "runs as its own phase AFTER your current turn ends: its role narration and "
            "final document will appear in this conversation, and you will get a follow-up "
            "turn to report the result. EXPENSIVE and SLOW (minutes, many model calls) -- "
            "use it only when the task needs a multi-role workflow the skill is designed "
            "for. Call it at most once per task; never call it again to 'check' the result.";
        def.parameters = {{"name", "string",
                           "Installed skill name (use the exact folder name; see /skill list).",
                           true},
                          {"topic", "string",
                           "What the skill should work on -- the user's task, in full.", true}};
        tools.registerTool(def, [this](const std::string& args) -> std::string {
            std::string name = extractStringArg(args, "name");
            std::string topic = extractStringArg(args, "topic");
            if (name.empty()) return "Error: Missing 'name' argument";
            if (topic.empty()) return "Error: Missing 'topic' argument";
            if (!modelSupportsTools(config.model))
                return "Error: model '" + config.model +
                       "' does not support tool calls; cannot run a skill";

            // Resolve case-insensitively through the same rule set the user
            // trigger uses (tolerate a leading "/" from the model).
            std::string guess = name;
            if (!guess.empty() && guess[0] == '/') guess.erase(0, 1);
            const auto resolved =
                skill_trigger::parse("/" + guess, SkillRunner::listSkills(skillsDir_));
            const std::string folder = resolved.kind == skill_trigger::Result::Kind::Trigger
                                           ? resolved.name
                                           : std::string();
            if (folder.empty()) {
                std::string avail;
                for (const auto& n : SkillRunner::listSkills(skillsDir_))
                    avail += "\n- " + n;
                return "Error: unknown skill '" + name +
                       "'. Installed skills:" + (avail.empty() ? "\n(none)" : avail);
            }

            {
                std::lock_guard<std::mutex> lk(skillMutex_);
                if (pendingSkill_)
                    return "Error: skill '" + pendingSkill_->name +
                           "' is already queued for this turn";
                if (lastSkillThisTurn_ == folder)
                    return "Error: skill '" + folder +
                           "' already ran in this turn. Its output "
                           "is above -- read the produced files and report it instead of "
                           "re-running the workflow.";
                if (skillCallsThisTurn_ >= 2)
                    return "Error: at most 2 skills per turn. Ask the user how to continue.";
                pendingSkill_ = PendingSkill{folder, topic};
                lastSkillThisTurn_ = folder;
                ++skillCallsThisTurn_;
                skillStatus_ = "skill " + folder + " queued ...";
            }
            if (onStreamingTick) onStreamingTick();

            // End the turn after this tool round; runAgentTurnLoop picks the
            // skill up and resumes the conversation when it is done.
            tools.requestTurnEnd();
            return "[skill] '" + folder +
                   "' started. It runs as its own phase now; its "
                   "narration and final document will be above your next turn. Ending this "
                   "turn loses nothing.";
        });
    }

    pytoolMgr_.emplace(config.pythonPath, ws);
    pytoolMgr_->scanAndRegister(tools, procRunner_);

    // Wire the process runner so ToolRegistry::cancelAll() can interrupt a
    // running exec_shell / pytool subprocess (Agent::cancel() -> cancelAll()).
    tools.setProcessRunner(procRunner_);
}

bool TuiApp::initialize(IProcessRunner* procRunner) {
    procRunner_ = procRunner;
    loadTomlConfig(config, "");
    client.setConfig(config);
    setupTools();
    skillsDir_ = getProjvDir() + "/skills";
    SkillRunner::ensureDefaultSkills(skillsDir_);

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
    promptFiles_.erase(std::remove(promptFiles_.begin(), promptFiles_.end(), "compactor.md"),
                       promptFiles_.end());
    activePromptIndex_ = 0;
    for (size_t i = 0; i < promptFiles_.size(); ++i) {
        if (promptFiles_[i] == "coder.md") {
            activePromptIndex_ = static_cast<int>(i);
            break;
        }
    }
    std::string sp = loadPromptFile(promptFiles_[activePromptIndex_]);
    agent->setSystemPrompt(sp);

    // Sessions dir (lazy-create DB on first message).
    sessionsDir_ = getSessionsDir();
    std::error_code ec;
    fs::create_directories(sessionsDir_, ec);
    if (ec) {
        debugLogf("[TuiApp] Failed to create sessions dir '%s': %s", sessionsDir_.c_str(),
                  ec.message().c_str());
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
        if (onThemeChanged) onThemeChanged();
    };

    buildBubblesFromMessages();
    refreshModels(); // kick off model list fetch (async)
    return true;
}

void TuiApp::harvestSpeakers() {
    // The active skill's role display names are the registry of known speakers.
    // Harvested each frame (cheap, mutex-guarded) so it tracks skills as they
    // start; entries persist for the rest of the app run.
    for (const auto& a : skillAgentStatuses()) {
        if (!a.displayName.empty()) knownSpeakers_.insert(a.displayName);
    }
}

void TuiApp::buildBubblesFromMessages() {
    if (!agent) return;
    chatHistory.clear();
    lastMessageId_ = 0;
    harvestSpeakers();

    auto msgs = agent->getNewMessagesSince(0);
    for (const auto& msg : msgs) {
        auto derived = bubble_model::deriveBubbles(msg, knownSpeakers_);
        chatHistory.insert(chatHistory.end(), derived.begin(), derived.end());
        if (msg.id > lastMessageId_) lastMessageId_ = msg.id;
    }
}

void TuiApp::syncChatFromAgent() {
    if (!agent) return;
    harvestSpeakers();

    auto newMsgs = agent->getNewMessagesSince(lastMessageId_);
    if (!newMsgs.empty()) {
        resetChatScroll(); // new content -> scroll back to bottom
    }
    for (const auto& msg : newMsgs) {
        auto derived = bubble_model::deriveBubbles(msg, knownSpeakers_);
        chatHistory.insert(chatHistory.end(), derived.begin(), derived.end());
        if (msg.id > lastMessageId_) lastMessageId_ = msg.id;
    }

    // Cap visible bubbles at 200 to bound rendering cost.
    constexpr size_t kMaxBubbles = 200;
    if (chatHistory.size() > kMaxBubbles) {
        size_t excess = chatHistory.size() - kMaxBubbles;
        chatHistory.erase(chatHistory.begin(), chatHistory.begin() + excess);
    }

    // Send any queued message once the previous turn has finished.
    drainPendingQueue();
}

void TuiApp::sendMessage(const std::string& text) {
    if (!agent || text.empty()) return;

    // Busy: queue the message instead of dropping it (drained on completion).
    if (agentThreadRunning_.load()) {
        pendingQueue_.push_back(text);
        return;
    }

    // Ensure the session DB exists so quick commands (/help etc.) persist.
    if (agent->ensureStorage) agent->ensureStorage();

    std::string trimmed = text;
    auto s = trimmed.find_first_not_of(" \t\r\n");
    auto e = trimmed.find_last_not_of(" \t\r\n");
    trimmed = (s == std::string::npos) ? std::string() : trimmed.substr(s, e - s + 1);

    // /workspace: frontend-only display of the workspace path (like legacy).
    if (trimmed == "/workspace") {
        std::string ws = config.workspacePath;
        if (ws.empty()) ws = fs::absolute(fs::path(getExeDir())).string();
        bubble_model::Bubble cb;
        cb.role = "system";
        cb.content = "[Workspace] " + ws;
        chatHistory.push_back(cb);
        resetChatScroll();
        return;
    }

    if (trimmed == "/compress") {
        agent->addPersistedMessage(Message::User("/compress"));
        agent->requestCompact();
        launchAgentThread();
        return;
    }

    // /<skill> <topic>: direct skill trigger (rules live in core/skill_trigger,
    // unit-tested). An unknown token resolves to Kind::None and falls through as
    // an ordinary message, so /usr/bin paths, "//" comments and natural language
    // are never intercepted. "/skill list" stays as the safe listing entry point;
    // the old "/skill <name> <topic>" form is gone. This branch runs BEFORE the
    // vision "@path" parsing below, so "/monica D:\dir @C:\a.png" hands the whole
    // line to the skill engine.
    {
        const auto installed = SkillRunner::listSkills(skillsDir_);
        const auto trig = skill_trigger::parse(trimmed, installed);

        if (trig.kind == skill_trigger::Result::Kind::List) {
            std::string msg = "[skill] available:";
            for (const auto& n : installed)
                msg += "\n- " + n;
            if (installed.empty())
                msg += "\n(none)";
            else
                msg += "\n\nUsage: /<skill> <topic>";
            bubble_model::Bubble cb;
            cb.role = "system";
            cb.content = msg;
            chatHistory.push_back(cb);
            resetChatScroll();
            return;
        }

        if (trig.kind == skill_trigger::Result::Kind::Trigger) {
            if (trig.topic.empty()) {
                bubble_model::Bubble cb;
                cb.role = "system";
                cb.content = "[skill] usage: /" + trig.name + " <topic>";
                chatHistory.push_back(cb);
                resetChatScroll();
                return;
            }
            // Skills carry no image input yet: say so instead of silently
            // dropping the attachments.
            if (trimmed.find('@') != std::string::npos) {
                bubble_model::Bubble cb;
                cb.role = "system";
                cb.content = "[image] images ignored: skills do not carry "
                             "image input yet.";
                chatHistory.push_back(cb);
            }
            startSkill(trig.name, trig.topic, trimmed);
            resetChatScroll();
            return;
        }
    }

    // Other quick commands handled by the backend (/clear /help).
    if (agent->handleQuickCommand(trimmed)) {
        // Already persisted; the event that sent it triggers a redraw.
        return;
    }

    // "@<path>" image markers (vision). Runs after the skill branch, so a
    // "/monica ... @C:\a.png" line was already handed to the skill engine.
    std::string turnText = trimmed;
    std::vector<ImageAttachment> images;
    {
        auto parsed = image_input::parseMarkers(trimmed, config.workspacePath);
        if (!parsed.ok()) {
            bubble_model::Bubble cb;
            cb.role = "system";
            cb.content = parsed.error + "\n(message not sent)";
            chatHistory.push_back(cb);
            resetChatScroll();
            return;
        }
        if (!parsed.images.empty()) {
            if (!modelSupportsVision(config.model)) {
                bubble_model::Bubble cb;
                cb.role = "system";
                cb.content = "[image] model '" + config.model +
                             "' does not accept image input. Switch to a Flash model (F10).\n"
                             "(message not sent)";
                chatHistory.push_back(cb);
                resetChatScroll();
                return;
            }
            images = std::move(parsed.images);
            turnText = parsed.text;
            // Markers-only message: give the turn something to answer.
            if (turnText.empty()) turnText = "Describe the attached image(s).";
        }
    }

    agent->startTurn(turnText, std::move(images));
    launchAgentThread();
}

void TuiApp::launchAgentThread() {
    if (agentThreadRunning_.load()) return;
    joinAgentThread();
    agentThreadRunning_.store(true);
    agentThread_ = std::thread([this]() {
        runAgentTurnLoop();
        agentThreadRunning_.store(false);
        if (onTurnComplete) onTurnComplete();
    });
}

// One user turn, then (optionally) the skill phase run_skill asked for, then a
// follow-up turn so the model can report the result. The skill itself goes
// through runSkillBlocking() -- the same function the "/<skill> <topic>" trigger
// uses -- so bubbles, the agent dock, the session and the DB all behave alike.
void TuiApp::runAgentTurnLoop() {
    skillCallsThisTurn_ = 0;
    {
        std::lock_guard<std::mutex> lk(skillMutex_);
        lastSkillThisTurn_.clear();
    }

    for (;;) {
        agent->newTurn();
        agent->run();

        // ESC during the model turn: stop. newTurn() resets the cancel flags,
        // so this check has to happen before the next iteration starts.
        if (tools.isCancelled()) break;

        PendingSkill sk;
        {
            std::lock_guard<std::mutex> lk(skillMutex_);
            if (!pendingSkill_) break;
            sk = std::move(*pendingSkill_);
            pendingSkill_.reset();
        }

        const bool skillOk = runSkillBlocking(sk.name, sk.topic, std::string());

        // ESC during the skill run: do not resume the conversation.
        if (tools.isCancelled()) break;

        // One-shot System message for the follow-up turn. Kept as short as the
        // hard rules in the system prompt: three bullets, no restating.
        agent->setTurnNudge(
            skillOk ? "[skill done] '" + sk.name +
                          "' finished -- its narration and final "
                          "document are the messages above.\n"
                          "- Read the produced files if you need exact details.\n"
                          "- Do not call run_skill again; report what came out.\n"
                          "- 3-5 lines: result, where it is, single next step. Point at the "
                          "document above instead of repeating it.\n"
                    : "[skill stopped] '" + sk.name +
                          "' did not finish -- cancelled or failed.\n"
                          "- Do not retry unless the user asks.\n"
                          "- 3-5 lines: what happened, next step.\n");
        // Loop: one more turn consumes the nudge and reports the result.
    }

    {
        std::lock_guard<std::mutex> lk(skillMutex_);
        pendingSkill_.reset();
        lastSkillThisTurn_.clear();
    }
    skillCallsThisTurn_ = 0;
}

void TuiApp::joinAgentThread() {
    if (agentThread_.joinable()) {
        try {
            agentThread_.join();
        } catch (...) {
        }
    }
}

void TuiApp::joinSkillThread() {
    if (skillThread_.joinable()) {
        try {
            skillThread_.join();
        } catch (...) {
        }
    }
}

std::string TuiApp::getSkillStatus() const {
    std::lock_guard<std::mutex> lk(skillMutex_);
    return skillStatus_;
}

std::vector<SkillRunner::AgentSnapshot> TuiApp::skillAgentStatuses() const {
    std::lock_guard<std::mutex> lk(skillMutex_);
    if (!skillRunner_) return {};
    return skillRunner_->agentStatuses();
}

// The main session rendered as reference material for skill agents.
std::string TuiApp::buildSharedContext() const {
    if (!agent) return std::string();
    auto msgs = agent->getSession().getContextMessages();
    std::string b;
    for (const auto& m : msgs) {
        if (m.role == "system") continue;
        if (m.role == "user") {
            if (!m.content.empty()) b += "USER: " + m.content + "\n\n";
        } else if (m.role == "assistant") {
            if (!m.content.empty()) b += "ASSISTANT: " + m.content + "\n\n";
        } else if (m.role == "tool") {
            b += "TOOL(" + m.name + "): " + m.content + "\n\n";
        }
    }
    if (b.empty()) return std::string();
    return "[SHARED SESSION CONTEXT - the main agent's full conversation so far. "
           "Reference material only; use it to resolve vague references.]\n\n" +
           b;
}

// ----- single skill execution path (both triggers go through here) ----------
bool TuiApp::runSkillBlocking(const std::string& name, const std::string& topic,
                              const std::string& rawInput) {
    if (!agent) return false;
    if (agent->ensureStorage) agent->ensureStorage();

    // Persist the exact user line so history replays match what was typed.
    // Empty == agent-triggered (run_skill): the user never typed anything, so do
    // not invent a user bubble -- the "[Role] ..." narration is the evidence.
    if (!rawInput.empty()) agent->addPersistedMessage(Message::User(rawInput));
    {
        std::lock_guard<std::mutex> lk(skillMutex_);
        skillStatus_ = "Preparing skill...";
    }
    if (onStreamingTick) onStreamingTick();

    SkillRunner::Callbacks cbs;
    cbs.onEvent = [this](const std::string& role, const std::string& kind,
                         const std::string& text) {
        std::string content;
        if (kind == "vote")
            content = "**[" + role + " vote]** " + text;
        else if (kind == "document")
            content = text;
        else
            content = "**[" + role + "]** " + text; // message / system
        if (agent) agent->addPersistedMessage(Message::Assistant(content));
    };
    cbs.onProgress = [this](const std::string& st) {
        {
            std::lock_guard<std::mutex> lk(skillMutex_);
            skillStatus_ = st;
        }
        if (onStreamingTick) onStreamingTick();
    };

    auto runner =
        std::make_unique<SkillRunner>(config, &tools, cbs, buildSharedContext(), skillsDir_);
    runner->setCancelProbe([this]() { return tools.isCancelled(); });

    SkillRunner* r = runner.get();
    {
        // Publish under the lock so the dock (skillAgentStatuses) and ESC
        // (cancelTurn) can reach this runner while it runs.
        std::lock_guard<std::mutex> lk(skillMutex_);
        skillRunner_ = std::move(runner);
    }

    try {
        r->run(name, topic);
    } catch (...) {
    }

    {
        std::lock_guard<std::mutex> lk(skillMutex_);
        skillRunner_.reset();
        skillStatus_.clear();
    }
    if (onTurnComplete) onTurnComplete();
    return !tools.isCancelled();
}

void TuiApp::startSkill(const std::string& name, const std::string& topic,
                        const std::string& rawInput) {
    if (!agent) return;
    const std::string persisted = rawInput.empty() ? ("/" + name + " " + topic) : rawInput;
    if (agentThreadRunning_.load()) {
        pendingQueue_.push_back(persisted);
        return;
    }
    if (agent->ensureStorage) agent->ensureStorage();

    joinSkillThread();
    agentThreadRunning_.store(true);
    skillThread_ = std::thread([this, name, topic, persisted]() {
        runSkillBlocking(name, topic, persisted);
        agentThreadRunning_.store(false);
        if (onTurnComplete) onTurnComplete();
    });
}

void TuiApp::drainPendingQueue() {
    if (agentThreadRunning_.load()) return;
    if (pendingQueue_.empty()) return;
    std::string next = pendingQueue_.front();
    pendingQueue_.erase(pendingQueue_.begin());
    sendMessage(next);
}

void TuiApp::cancelTurn() {
    pendingQueue_.clear();
    {
        std::lock_guard<std::mutex> lk(skillMutex_);
        pendingSkill_.reset(); // drop a queued-but-not-started skill
        if (skillRunner_) skillRunner_->cancel();
    }
    if (agent) agent->cancel();
}

void TuiApp::shutdown() {
    {
        std::lock_guard<std::mutex> lk(skillMutex_);
        if (skillRunner_) skillRunner_->cancel();
    }
    joinSkillThread();
    if (agent) agent->cancel();
    joinAgentThread();
    joinModelsThread();
    tools.setProcessRunner(nullptr); // detach before main deletes the runner
    delete agent;
    agent = nullptr;
    // Explicitly close the DB and checkpoint the WAL on exit (bug fix).
    storage.closeDatabase();
}

void TuiApp::emergencyShutdown() {
    {
        std::lock_guard<std::mutex> lk(skillMutex_);
        if (skillRunner_) skillRunner_->cancel();
    }
    joinSkillThread();
    if (agent) agent->cancel();
    joinAgentThread();
    // Do not join the models thread: it never touches the DB and might be
    // blocked on the network. Close the DB now; the OS reclaims the rest.
    storage.closeDatabase();
}

status_bar::Data TuiApp::getStatusBarData() const {
    status_bar::Data d;
    d.model = config.model;
    d.promptTokens = totalPromptTokens_.load(std::memory_order_relaxed);
    d.completionTokens = totalCompletionTokens_.load(std::memory_order_relaxed);
    d.status = getStatus();
    d.workspace = config.workspacePath;
    d.cost = totalPromptTokens_.load() * lookupModelPrice(config.modelPrices, config.model, true) *
                 1e-6 +
             totalCompletionTokens_.load() *
                 lookupModelPrice(config.modelPrices, config.model, false) * 1e-6;
    if (activePromptIndex_ >= 0 && activePromptIndex_ < static_cast<int>(promptFiles_.size())) {
        d.roleName = promptFiles_[activePromptIndex_];
        if (d.roleName.size() > 3 && d.roleName.substr(d.roleName.size() - 3) == ".md") {
            d.roleName = d.roleName.substr(0, d.roleName.size() - 3);
        }
    }
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

void TuiApp::saveFullConfig(const std::string& apiKey, const std::string& baseUrl, int maxTokens,
                            double temperature, const std::string& workspace,
                            const std::string& compiler, const std::string& python,
                            const std::string& model) {
    config.apiKey = apiKey;
    config.baseUrl = baseUrl;
    config.maxTokens = maxTokens;
    config.temperature = temperature;
    config.workspacePath = workspace;
    config.cppCompilerPath = compiler;
    config.pythonPath = python;
    config.model = model;
    config.loaded = true;
    config.loadError.clear();

    if (!saveConfig(config)) {
        config.loadError = "Failed to save config file!";
    }

    client.setConfig(config);
    setupTools(); // re-register tools with new workspace path
    agent->setToolPaths(config.cppCompilerPath, config.pythonPath);
    agent->setRequestParams(config.maxTokens, config.temperature);
    agent->setModel(config.model);
    agent->setContextWindow(0); // reset auto-detect on model switch
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

std::vector<std::string> TuiApp::listSessions() const {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(sessionsDir_, ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        if (!entry.is_regular_file(ec)) continue;
        if (entry.path().extension() != ".db") continue;
        out.push_back(entry.path().string());
    }
    // Timestamp-named files: newest first.
    std::sort(out.begin(), out.end(), std::greater<std::string>());
    return out;
}

void TuiApp::switchPrompt(int index) {
    if (!agent || agentThreadRunning_.load()) return; // only while idle
    if (index < 0 || index >= static_cast<int>(promptFiles_.size())) return;
    if (index == activePromptIndex_) return;
    activePromptIndex_ = index;
    std::string content = loadPromptFile(promptFiles_[activePromptIndex_]);
    agent->replaceSystemPrompt(content);
}

void TuiApp::setModel(const std::string& model) {
    config.model = model;
    if (agent) {
        agent->setModel(model);
        agent->setContextWindow(0); // reset auto-detect on model switch
    }
    saveConfig(config);
}

void TuiApp::refreshModels() {
    if (modelsLoading_.exchange(true)) return; // already loading
    joinModelsThread();
    modelsThread_ = std::thread([this]() {
        std::string err;
        auto models = client.fetchModels(&err);
        if (!models.empty()) {
            std::lock_guard<std::mutex> lk(modelsMutex_);
            availableModels_ = std::move(models);
        }
        modelsLoading_.store(false);
        if (onModelsUpdated) onModelsUpdated();
    });
}

std::vector<ModelInfo> TuiApp::getAvailableModels() const {
    std::lock_guard<std::mutex> lk(modelsMutex_);
    return availableModels_;
}

void TuiApp::joinModelsThread() {
    if (modelsThread_.joinable()) {
        try {
            modelsThread_.join();
        } catch (...) {
        }
    }
}

void TuiApp::toggleReasoning() {
    reasoningExpanded_ = !reasoningExpanded_;
}

void TuiApp::scrollChat(int delta) {
    int maxScroll =
        chatContentRows_ > chatViewportRows_ ? chatContentRows_ - chatViewportRows_ - 1 : 0;
    if (chatFollowBottom_) {
        chatScrollRow_ = maxScroll; // snap to the current bottom before scrolling
        chatFollowBottom_ = false;
    }
    chatScrollRow_ -= delta; // delta > 0 = scroll up (row index decreases)
    if (chatScrollRow_ < 0) chatScrollRow_ = 0;
    if (chatScrollRow_ >= maxScroll) {
        chatScrollRow_ = maxScroll;
        chatFollowBottom_ = true;
    }
}

void TuiApp::resetChatScroll() {
    chatFollowBottom_ = true;
    chatScrollRow_ = 0;
}

int TuiApp::chatFocusRow() const {
    return chatFollowBottom_ ? 1000000000 : chatScrollRow_ + chatViewportRows_ / 2;
}
