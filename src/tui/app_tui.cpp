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
    // The workspace is the launch directory (getWorkspaceDir): every file tool,
    // the shell CWD and the pytool sandbox root there.
    std::string ws = getWorkspaceDir();

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
            "for. Different skills may be chained in one turn (at most 4 per turn); never "
            "re-run the same skill to 'check' its result -- read the produced files instead.";
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
                if (!turnRequest_.claim(turn_requests::Kind::Skill))
                    return std::string("Error: ") + turnRequest_.refusedMessage();
                if (lastSkillThisTurn_ == folder) {
                    turnRequest_.release();
                    return "Error: skill '" + folder +
                           "' already ran in this turn. Its output "
                           "is above -- read the produced files and report it instead of "
                           "re-running the workflow.";
                }
                if (skillCallsThisTurn_ >= 4) {
                    turnRequest_.release();
                    return "Error: at most 4 skills per turn. Ask the user how to continue.";
                }
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

    // view_image: the model asks to look at images on disk. The tool returns no
    // pixels -- it registers an image round and ends the turn, exactly like
    // run_skill. runAgentTurnLoop then appends a synthetic user message carrying
    // the attachments (the API only accepts images in user messages), so the next
    // turn can actually see them.
    {
        ToolDefinition def;
        def.name = "view_image";
        def.description =
            "Look at images stored on disk (screenshots, diagrams, photos). The images "
            "arrive in the NEXT turn as a user message, because this tool ends the current "
            "turn -- so call it alone: do not batch it with file edits, shell commands or "
            "run_skill. Other tools in the same round still execute and their results are "
            "kept, but the round stops afterwards. Requires a vision model (the Flash "
            "line); on other models it returns an error.";
        def.parameters = {
            {"paths", "array",
             "Image files or directories (top-level images expand). Absolute paths, or "
             "paths relative to the workspace; must stay inside the workspace.",
             true}};
        tools.registerTool(def, [this, ws](const std::string& args) -> std::string {
            if (!modelSupportsVision(config.model))
                return "Error: model '" + config.model +
                       "' cannot read images. Switch to a Flash model (F10) first.";

            const std::vector<std::string> rawPaths = extractStringArrayArg(args, "paths");
            if (rawPaths.empty()) return "Error: missing 'paths' array";

            // Same workspace guard as every other file tool, then the same
            // count / size / total / mime checks the "@path" markers use.
            std::vector<std::string> resolved;
            resolved.reserve(rawPaths.size());
            for (const auto& p : rawPaths) {
                std::string full;
                std::string pathErr = validateToolPath(p, ws, full);
                if (!pathErr.empty()) return pathErr;
                resolved.push_back(full);
            }
            std::vector<ImageAttachment> images;
            std::string imgErr;
            if (!image_input::loadAttachments(resolved, ws, images, imgErr)) return imgErr;

            const size_t count = images.size();
            {
                std::lock_guard<std::mutex> lk(skillMutex_);
                if (!turnRequest_.claim(turn_requests::Kind::Image))
                    return std::string("Error: ") + turnRequest_.refusedMessage();
                pendingImages_ = PendingImages{std::move(images)};
            }
            if (onStreamingTick) onStreamingTick();

            // End the turn after this tool round; runAgentTurnLoop injects the
            // images and starts the follow-up turn.
            tools.requestTurnEnd();
            return "[image] " + std::to_string(count) +
                   " image(s) queued. They arrive as a user message in the next turn; this "
                   "turn ends here, nothing is lost.";
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
    agent->setReasoningEffort(config.reasoningEffort);
    agent->setWorkspacePath(getWorkspaceDir());
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

    // Fixed workspace context: directory tree + AGENTS.md. Same call the F3 and
    // "/clear" paths use, so a conversation always starts with the same prefix.
    injectWorkspaceContext();

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
        std::string ws = getWorkspaceDir();
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

    // "/clear" is handled here rather than in Agent: wiping the session must also
    // restore the fixed system prompt and the workspace context, and the core has
    // no access to the prompt files. Same path as F3, except the session database
    // (and the visible history) stay as they are.
    if (trimmed == "/clear") {
        reinitConversation(true);
        agent->addPersistedMessage(Message::System(
            "[clear] context cleared. System prompt, directory tree and AGENTS.md were "
            "reloaded; the visible history is kept, the model starts fresh."));
        bubble_model::Bubble cb;
        cb.role = "system";
        cb.content = "[clear] context cleared; prompt and workspace context reloaded.";
        chatHistory.push_back(cb);
        resetChatScroll();
        return;
    }

    // Other quick commands handled by the backend (/help).
    if (agent->handleQuickCommand(trimmed)) {
        // Already persisted; the event that sent it triggers a redraw.
        return;
    }

    // "@<path>" image markers (vision). Runs after the skill branch, so a
    // "/monica ... @C:\a.png" line was already handed to the skill engine.
    std::string turnText = trimmed;
    std::vector<ImageAttachment> images;
    {
        auto parsed = image_input::parseMarkers(trimmed, getWorkspaceDir());
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
        turnRequest_.release();
    }

    for (;;) {
        agent->newTurn();
        agent->run();

        // ESC during the model turn: stop. newTurn() resets the cancel flags,
        // so this check has to happen before the next iteration starts.
        if (tools.isCancelled()) break;

        // An image round (view_image) wins over a skill if both somehow queued;
        // turnRequest_ makes that impossible, this is just the consume order.
        std::optional<PendingImages> images;
        {
            std::lock_guard<std::mutex> lk(skillMutex_);
            if (pendingImages_) {
                images = std::move(pendingImages_);
                pendingImages_.reset();
                turnRequest_.release();
            }
        }
        if (images) {
            // Synthetic user message: images are only legal in a user message, so
            // the framework speaks on the model's behalf. No turn nudge -- after
            // this round the model should keep working like after any user message.
            Message msg = Message::User("View the attached image(s) and continue the task.");
            msg.images = std::move(images->images);
            msg.synthetic = true;
            agent->addPersistedMessage(msg);
            if (tools.isCancelled()) break;
            continue; // next iteration calls newTurn() + run()
        }

        PendingSkill sk;
        {
            std::lock_guard<std::mutex> lk(skillMutex_);
            if (!pendingSkill_) break;
            sk = std::move(*pendingSkill_);
            pendingSkill_.reset();
            turnRequest_.release();
        }

        const bool skillOk = runSkillBlocking(sk.name, sk.topic, std::string());

        // ESC during the skill run: do not resume the conversation.
        if (tools.isCancelled()) break;

        // One-shot System message for the follow-up turn. A skill's output is
        // raw material, not a verdict: pipelines like avenger -> steve -> musk
        // need the main agent to review the produced document and say what is
        // weak before moving on, so the wrap-up is NOT capped to a few lines.
        agent->setTurnNudge(
            skillOk ? "[skill done] '" + sk.name +
                          "' finished -- its narration and final document are the messages above.\n"
                          "- Never call run_skill again for this same skill to 'check' its "
                          "result; read the produced files instead.\n"
                          "- Review what came out against what the user asked: what it decided, "
                          "what is weak or unproven, whether the document is complete.\n"
                          "- Then report normally -- as thorough as the task deserves: result, "
                          "where it is on disk, the real gaps, and the next step. A pipeline "
                          "stage that has not run yet should be named here.\n"
                    : "[skill stopped] '" + sk.name +
                          "' did not finish -- cancelled or failed.\n"
                          "- Do not retry unless the user asks.\n"
                          "- Say what happened, what was already produced, and the next step.\n");
        // Loop: one more turn consumes the nudge and reports the result.
    }

    {
        std::lock_guard<std::mutex> lk(skillMutex_);
        pendingSkill_.reset();
        pendingImages_.reset();
        turnRequest_.release();
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
        pendingImages_.reset();
        turnRequest_.release();
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
    d.workspace = getWorkspaceDir();
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
                            double temperature, const std::string& compiler,
                            const std::string& python, const std::string& model) {
    config.apiKey = apiKey;
    config.baseUrl = baseUrl;
    config.maxTokens = maxTokens;
    config.temperature = temperature;
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
    agent->setReasoningEffort(config.reasoningEffort);
    agent->setModel(config.model);
    agent->setContextWindow(0); // reset auto-detect on model switch
    config.loadError.clear();
}

void TuiApp::newChat() {
    reinitConversation(false);
}

void TuiApp::injectWorkspaceContext() {
    if (!agent) return;
    // Directory tree: the LLM knows where files actually live instead of
    // guessing wrong paths.
    {
        std::string ctx = buildProjectContext(getWorkspaceDir());
        if (!ctx.empty()) {
            agent->addPersistedMessage(Message::System(ctx));
            debugLogf("[TuiApp] Injected project context (%zu chars)", ctx.size());
        }
    }
    // AGENTS.md: the repository owner's instructions for agents. Editing the file
    // takes effect in the next conversation (F3) or after "/clear".
    {
        std::string md = buildAgentsMdContext(getWorkspaceDir());
        if (!md.empty()) {
            agent->addPersistedMessage(Message::System(md));
            debugLogf("[TuiApp] Injected AGENTS.md (%zu chars)", md.size());
        }
    }
}

void TuiApp::reinitConversation(bool keepStorage) {
    if (!agent) return;
    agent->cancel();
    if (!keepStorage) {
        if (storage.isOpen()) storage.closeDatabase();
        agent->setStorage(nullptr);
        totalPromptTokens_.store(0, std::memory_order_relaxed);
        totalCompletionTokens_.store(0, std::memory_order_relaxed);
    }
    agent->clearSession();
    // System prompt first: buildChatRequest() inserts its dynamic system block
    // right after the fixed system messages, so the session must start with one.
    {
        std::string sp = loadPromptFile(promptFiles_[activePromptIndex_]);
        agent->setSystemPrompt(sp);
        agent->addPersistedMessage(Message::System(sp));
    }
    injectWorkspaceContext();
    if (keepStorage) {
        // "/clear" keeps the session file and the on-screen history; the model
        // simply starts from an empty context again.
        return;
    }
    chatHistory.clear();
    lastMessageId_ = 0;
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
