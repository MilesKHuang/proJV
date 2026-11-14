// proJV TUI -- application shell implementation (backend wiring).
#include "app_tui.h"

#include "core/prompts.h"
#include "core/config.h"
#include "core/project_context.h"
#include "core/context_assembler.h"
#include "core/image_input.h"
#include "core/skill_trigger.h"
#include "core/terminal_status.h"
#include "core/snapshot.h"
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
    // Two roots, not one: the workspace is the launch directory (every file tool,
    // the shell CWD and the pytool sandbox root there), and projv_files sits next
    // to the exe. projv_files is writable -- that is what "add a skill" means --
    // except sessions/ and logs/, which are history and stay read-only.
    paths_ = ToolPaths::forSession(getWorkspaceDir(), getProjvDir());
    std::string ws = getWorkspaceDir();
    if (agent) agent->setWorkspacePath(paths_.describe());

    // Snapshots (/undo). Best effort: an oversized workspace or a read-only
    // store turns undo off with a reason, and the rest of the app keeps going.
    {
        std::string snapErr;
        if (!snapshot::open(ws, config.snapshotsKeepDays, config.snapshotsMaxWorkspaceGb,
                            snapErr)) {
            debugLogWarnf("[TuiApp] undo disabled: %s", snapErr.c_str());
        }
    }

    registerShellTool(tools, paths_, procRunner_, &taskRegistry_);
    registerTaskOutputTool(tools, &taskRegistry_);
    registerFileTools(tools, paths_);
    registerFormatTool(tools, paths_, procRunner_, config.clangFormatPath);
    registerMdFileTool(tools, paths_);
    registerEditFileTool(tools, paths_);
    registerFileSearchTool(tools, paths_);
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
                // The queue always accepts; the runtime drains it in order.
                turnRequest_.claim(turn_requests::Kind::Skill);
                if (lastSkillThisTurn_ == folder) {
                    turnRequest_.drop(turn_requests::Kind::Skill);
                    return "Error: skill '" + folder +
                           "' already ran in this turn. Its output "
                           "is above -- read the produced files and report it instead of "
                           "re-running the workflow.";
                }
                if (skillCallsThisTurn_ >= 4) {
                    turnRequest_.drop(turn_requests::Kind::Skill);
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
                std::string pathErr = validateToolPath(paths_, p, false, full);
                if (!pathErr.empty()) return pathErr;
                resolved.push_back(full);
            }
            std::vector<ImageAttachment> images;
            std::string imgErr;
            if (!image_input::loadAttachments(resolved, ws, images, imgErr)) return imgErr;

            const size_t count = images.size();
            {
                std::lock_guard<std::mutex> lk(skillMutex_);
                // Image jumps the queue: the model cannot continue without it.
                turnRequest_.claim(turn_requests::Kind::Image);
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
    // OSC 7501: tell the hosting terminal what we are doing. No-op when the
    // config switch is off, PROJV_NO_OSC=1, or stdout is not a TTY.
    terminal_status::init(config.oscStatus);
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
    // Mirror the agent loop into the host terminal. This runs on the agent
    // thread, so it only records the state; the UI thread writes it (see
    // terminal_status::flushIfDirty in the render callback).
    agent->setPhaseCallback([this](AgentPhase p, const std::string& msg) {
        switch (p) {
        case AgentPhase::Streaming:
        case AgentPhase::ExecutingTools:
            terminal_status::working(msg);
            break;
        case AgentPhase::AwaitApproval:
            // blocked+permission is the one record a host must surface: a human
            // is genuinely required. It is the only blocking reason proJV has.
            terminal_status::blocked("permission", msg);
            break;
        case AgentPhase::Error:
            terminal_status::error(msg);
            break;
        case AgentPhase::Idle:
            terminal_status::done(msg);
            break;
        }
        if (onStreamingTick) onStreamingTick(); // wake the UI thread
    });
    agent->setRequestParams(config.maxTokens, config.temperature);
    agent->setReasoningEffort(config.reasoningEffort);
    agent->setApprovalMode(config.approvalMode);
    agent->setPersistentGrants(config.approvalGrants);
    // ALWAYS -> write it into config.toml. Core has no config access, so the TUI
    // owns persistence; this runs on the agent thread right after the click.
    agent->onGrantPersist = [this](const std::string& cmd) {
        if (std::find(config.approvalGrants.begin(), config.approvalGrants.end(), cmd) ==
            config.approvalGrants.end())
            config.approvalGrants.push_back(cmd);
        saveConfig(config);
        debugLogf("[TuiApp] approval grant persisted: %s", cmd.c_str());
    };
    agent->setWorkspacePath(paths_.describe());
    agent->setModel(config.model);
    agent->setContextWindow(config.contextWindow);

    // Prompt files from projv_files/prompts/.
    promptFiles_ = ensureDefaultPrompts();
    // compactor.md is the /compress prompt, not a role. Every other file here is
    // a role, whatever it is called -- no name is reserved.
    promptFiles_.erase(std::remove(promptFiles_.begin(), promptFiles_.end(), "compactor.md"),
                       promptFiles_.end());
    activePromptIndex_ = defaultPromptIndex(promptFiles_);
    std::string sp = activePromptContent();
    agent->setSystemPrompt(sp);

    // Sessions dir (lazy-create DB on first message).
    sessionsDir_ = getSessionsDir();
    std::error_code ec;
    fs::create_directories(sessionsDir_, ec);
    if (ec) {
        debugLogWarnf("[TuiApp] Failed to create sessions dir '%s': %s", sessionsDir_.c_str(),
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
            debugLogErrf("[TuiApp] Failed to lazy-create database: %s", dbPath.c_str());
        }
    };

    // Inject system prompt as first session message.
    agent->addPersistedMessage(Message::System(activePromptContent()));

    // Fixed prefix: P1 [ENVIRONMENT] + P2 [PROJECT] right after the role prompt.
    // Same call the F3 and "/clear" paths use, so a conversation always starts
    // with the same three system messages.
    injectFixedBlocks();

    // MCP: read projv_files/mcp.json and spawn every enabled + trusted server.
    // The config file is created empty on first run, like ensureDefaultPrompts().
    // Spawning happens on per-server worker threads, so nothing here blocks the
    // UI; tools are registered at the first turn boundary (see runAgentTurnLoop),
    // which is also where the capability map that names them is built.
    mcpManager_ = std::make_unique<mcp::Manager>();
    mcpManager_->setWorkDir(getWorkspaceDir());
    mcp::ensureConfigFile(mcp::configPath());
    if (!mcpManager_->loadFromJson(mcp::configPath()))
        debugLogWarnf("[TuiApp] mcp.json: %s", mcpManager_->configError().c_str());
    mcpManager_->startEnabled();

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

    // Main thread, idle -- the only safe window to add to or erase from the MCP
    // server vector (the render loop walks it every frame, mcp_view.cpp). This is
    // what makes "edit mcp.json, then send a message" work without a restart.
    if (mcpManager_) {
        if (mcpManager_->reloadIfChanged(mcp::configPath()))
            debugLogf("[TuiApp] mcp.json reloaded before this turn");
        // Handles removed by the reload are unregistered by syncTools at the turn
        // boundary; only then is it safe to drop them from the pool.
        mcpManager_->reapRemoved();
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
        // /undo and /restore persist a [Snapshot] message; without this sync it
        // would stay invisible until the next turn ends, so the user would see
        // no feedback at all.
        syncChatFromAgent();
        return;
    }

    // /wish: start, inspect or cancel the wish mode. Starting it also starts a
    // turn -- a wish that only sets a flag would sit there doing nothing.
    if (trimmed == "/wish" || trimmed.rfind("/wish ", 0) == 0) {
        std::string arg = trimmed.size() > 6 ? trimmed.substr(6) : std::string();
        const size_t b = arg.find_first_not_of(" \t");
        arg = (b == std::string::npos) ? std::string() : arg.substr(b);

        if (arg.empty()) {
            const Wish w = agent->wishSnapshot();
            std::string msg;
            if (!w.active) {
                msg = "[wish] not active. Usage: /wish <what you want built>";
            } else {
                msg = "[wish] active: " + w.objective + "\n" + std::to_string(w.passes) +
                      " pass(es), " + std::to_string(w.decisions) + " decision(s), " +
                      (agent->hasUnfinishedTodo() ? "TODO still open" : "TODO clear") +
                      ".\n/wish cancel stops it; the TODO list is kept.";
            }
            bubble_model::Bubble cb;
            cb.role = "system";
            cb.content = msg;
            chatHistory.push_back(cb);
            resetChatScroll();
            return;
        }
        if (arg == "cancel") {
            agent->cancelWish();
            bubble_model::Bubble cb;
            cb.role = "system";
            cb.content = "[wish] cancelled. The TODO list and the recorded decisions are kept.";
            chatHistory.push_back(cb);
            resetChatScroll();
            return;
        }
        agent->startWish(arg);
        bubble_model::Bubble cb;
        cb.role = "system";
        cb.content = "[wish] started: " + arg +
                     "\nThe model decides the open questions itself and keeps working until "
                     "its TODO list is empty. /wish cancel stops it.";
        chatHistory.push_back(cb);
        resetChatScroll();
        agent->startTurn(arg, std::vector<ImageAttachment>());
        launchAgentThread();
        return;
    }

    // /end_task <id> | all: stop a background task. Killing is deliberate: ESC only
    // detaches from a wait, it never kills a build the user started on purpose.
    if (trimmed == "/end_task" || trimmed.rfind("/end_task ", 0) == 0) {
        std::string arg = trimmed.size() > 9 ? trimmed.substr(9) : std::string();
        const size_t b = arg.find_first_not_of(" \t");
        arg = (b == std::string::npos) ? std::string() : arg.substr(b);
        std::string msg;
        if (arg.empty()) {
            const auto snaps = taskRegistry_.snapshots();
            if (snaps.empty())
                msg = "[bg] no background tasks in this session.";
            else {
                msg = "[bg] tasks:";
                for (const auto& s : snaps)
                    msg += "\n- bg#" + std::to_string(s.id) + " " + bg_task::stateLabel(s.state) +
                           (s.exitCode >= 0 ? " (exit " + std::to_string(s.exitCode) + ")" : "") +
                           " -- " + s.command;
                msg += "\nUsage: /end_task <id> or /end_task all";
            }
        } else if (arg == "all") {
            const size_t n = taskRegistry_.endAll();
            msg = "[bg] stopped " + std::to_string(n) + " task(s).";
        } else {
            std::string err;
            const int id = std::atoi(arg.c_str());
            if (id <= 0)
                msg = "[bg] '" + arg + "' is not a task id.";
            else if (taskRegistry_.end(id, err))
                msg = "[bg] bg#" + std::to_string(id) + " stopped.";
            else
                msg = "[bg] " + err + ".";
        }
        bubble_model::Bubble cb;
        cb.role = "system";
        cb.content = msg;
        chatHistory.push_back(cb);
        resetChatScroll();
        return;
    }

    // /approval: what the policy is and what has been remembered. Without this the
    // persisted grants are invisible, and an invisible allow-list is a bad one.
    if (trimmed == "/approval" || trimmed.rfind("/approval ", 0) == 0) {
        std::string arg = trimmed.size() > 9 ? trimmed.substr(9) : std::string();
        const size_t b = arg.find_first_not_of(" \t");
        arg = (b == std::string::npos) ? std::string() : arg.substr(b);
        std::string msg;
        if (arg == "clear") {
            agent->clearGrants();
            config.approvalGrants.clear();
            saveConfig(config);
            msg = "[approval] remembered commands cleared.";
        } else if (!arg.empty() && arg != "list") {
            msg = "[approval] usage: /approval, /approval clear";
        } else {
            msg = "[approval] mode: " + approvalModeName(agent->approvalMode()) +
                  "\n  strict   -- every delete/overwrite asks, no ALWAYS\n"
                  "  balanced -- destructive asks, build output runs (default)\n"
                  "  loose    -- only format/partition is refused\n";
            const auto grants = agent->grants();
            if (grants.empty()) {
                msg += "remembered: (none)";
            } else {
                msg += "remembered (" + std::to_string(grants.size()) + "):";
                for (const auto& g : grants)
                    msg += "\n- " + g;
                msg += "\n/approval clear forgets them all.";
            }
        }
        bubble_model::Bubble cb;
        cb.role = "system";
        cb.content = msg;
        chatHistory.push_back(cb);
        resetChatScroll();
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
        // --- Turn boundary (the only writer's window) ----------------------
        // Both calls below mutate state the Agent thread reads while building a
        // request (ToolRegistry's bare unordered_maps, Agent's auto-tool set).
        // Running them here -- on the agent thread, with no turn in flight -- is
        // what lets the registry stay lock-free.
        if (toolsReloadRequested_.exchange(false)) {
            debugLogf("[TuiApp] applying deferred tool reload (config saved mid-turn)");
            setupTools();
            applyConfigToAgent();
        }
        if (mcpManager_) {
            const mcp::Manager::SyncResult res = mcpManager_->syncTools(tools);
            if (!res.added.empty()) agent->addAutoTools(res.added);
            if (!res.removed.empty()) agent->removeAutoTools(res.removed);
        }
        // P3 after the registry is settled: the capability map must name exactly
        // the tools the request will carry. Building it earlier (before syncTools)
        // would miss a server that became Ready during the last turn.
        refreshCapabilitiesBlock();

        agent->newTurn();
        agent->run();

        // ESC during the model turn: stop. newTurn() resets the cancel flags,
        // so this check has to happen before the next iteration starts.
        if (tools.isCancelled()) break;

        // Image first: turn_requests pushes it to the front of the queue.
        std::optional<PendingImages> images;
        {
            std::lock_guard<std::mutex> lk(skillMutex_);
            if (pendingImages_) {
                images = std::move(pendingImages_);
                pendingImages_.reset();
                turnRequest_.drop(turn_requests::Kind::Image);
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
            if (pendingSkill_) {
                sk = std::move(*pendingSkill_);
                pendingSkill_.reset();
                turnRequest_.drop(turn_requests::Kind::Skill);
            }
        }

        if (!sk.name.empty()) {
            const bool skillOk = runSkillBlocking(sk.name, sk.topic, std::string());

            // ESC during the skill run: do not resume the conversation.
            if (tools.isCancelled()) break;

            // One-shot System message for the follow-up turn. A skill's output is
            // raw material, not a verdict: pipelines like avenger -> steve -> musk
            // need the main agent to review the produced document and say what is
            // weak before moving on, so the wrap-up is NOT capped to a few lines.
            agent->setTurnNudge(
                skillOk
                    ? "[skill done] '" + sk.name +
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
            // One more pass consumes the nudge; step 1 of the next pass, so the wish
            // checks below must not cut the skill's report turn short.
            continue;
        }

        // 1b. Background tasks that finished since the last check. This outranks the
        // wish: the model asked to be told, and the output is only useful while the
        // instruction that started it is still in context.
        {
            const std::vector<int> done = taskRegistry_.takeFinished();
            if (!done.empty()) {
                std::string msg = "[background] ";
                for (size_t i = 0; i < done.size(); ++i) {
                    if (i) msg += "\n[background] ";
                    const auto snaps = taskRegistry_.snapshots();
                    const bg_task::Snapshot* s = nullptr;
                    for (const auto& cand : snaps)
                        if (cand.id == done[i]) s = &cand;
                    msg += "bg#" + std::to_string(done[i]);
                    if (s)
                        msg += " " + std::string(bg_task::stateLabel(s->state)) + " (exit " +
                               std::to_string(s->exitCode) + "), " +
                               std::to_string(s->outputBytes) + " bytes: " + s->command;
                    msg += "\nRead it with task_output(" + std::to_string(done[i]) + ").";
                }
                bubble_model::Bubble cb;
                cb.role = "system";
                cb.content = msg;
                chatHistory.push_back(cb);
                agent->addPersistedMessage(Message::System(msg));
                continue; // let the model react to the result
            }
        }

        // 2. A queued user message outranks the wish. Leaving it until the wish
        // finishes would hide it for hours, so break out here: the caller drains the
        // queue and starts a normal turn, and the wish state survives for it.
        if (pendingCount() > 0) break;
        // 3. Wish: keep working while the checklist is open. Nothing is injected --
        // the [WISH], [TODO] and [USER REQUEST ON-GOING] blocks are rebuilt from
        // state by buildChatRequest() on every pass.
        {
            const Wish w = agent->endWishPass();
            if (w.active) {
                const std::string stop = agent->wishStopReason(w);
                if (stop.empty()) continue;
                std::string msg = "**Wish stopped** (" + stop + ")\n";
                const auto decisions = agent->wishDecisions();
                if (!decisions.empty()) {
                    msg += "\nDecisions taken for you:\n";
                    for (const auto& d : decisions)
                        msg += "- " + d + "\n";
                }
                const std::string todo = agent->unfinishedTodoText();
                if (!todo.empty()) msg += "\nStill open:\n" + todo;
                bubble_model::Bubble cb;
                cb.role = "system";
                cb.content = msg;
                chatHistory.push_back(cb);
                resetChatScroll();
                agent->cancelWish();
            }
        }
        break;
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
std::string TuiApp::buildSharedContext() {
    if (!agent) return std::string();
    // A digest, not the transcript. The full conversation used to be pasted into
    // every sub-agent's prompt -- in a long session that is hundreds of thousands of
    // tokens per agent. The digest reuses the compactor (same prompt, same
    // overview + todo + slice input) and never touches the session.
    const std::string digest = agent->summarizeConversationForSharing();
    if (digest.empty()) return std::string();
    return "[SHARED SESSION CONTEXT - a summary of the main agent's session so far, not the raw "
           "log. Reference material only; use it to resolve vague references.]\n\n" +
           digest;
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
    // The main loop is Idle during a skill phase, so the state has to be
    // reported from here or the host would show "idle" while a debate runs.
    terminal_status::working("skill: " + name);
    if (onStreamingTick) onStreamingTick();

    SkillRunner::Callbacks cbs;
    int events = 0;
    cbs.onEvent = [this, &events](const std::string& role, const std::string& kind,
                                  const std::string& text) {
        ++events;
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

    // A skill that dies silently is the worst failure mode there is: the dock shows
    // it for a moment, nothing reaches the chat, and the Release log (WARNING+) has
    // no trace. Both halves are recorded here -- the exception text when it throws,
    // and "produced nothing" when it returns without a single event.
    std::string crash;
    try {
        r->run(name, topic);
    } catch (const std::exception& e) {
        crash = e.what();
    } catch (...) {
        crash = "unknown exception";
    }
    if (!crash.empty()) {
        debugLogErrf("[skill] %s crashed: %s (topic: %.120s)", name.c_str(), crash.c_str(),
                     topic.c_str());
        const std::string msg = "[skill] " + name + " crashed before it could report anything:\n" +
                                crash + "\n(topic: " + topic +
                                ")\nThe session is intact; fix the cause and re-run.";
        if (agent) agent->addPersistedMessage(Message::System(msg));
    } else if (events == 0) {
        debugLogWarnf("[skill] %s produced no event (topic: %.120s)", name.c_str(), topic.c_str());
        const std::string msg =
            "[skill] " + name + " finished without producing a single message (topic: " + topic +
            ").\nCommon causes: the topic is not something the skill can act on, the agent "
            "budget ran out before the first round, or the API rejected the first request. "
            "Check projv_files/logs/proJV-<date>.log and try a narrower topic.";
        if (agent) agent->addPersistedMessage(Message::System(msg));
    }

    {
        std::lock_guard<std::mutex> lk(skillMutex_);
        skillRunner_.reset();
        skillStatus_.clear();
    }
    terminal_status::done("skill: " + name + " finished");
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
    // A user-invoked skill starts un-cancelled. Agent::cancel() (ESC, F3, F5, /clear)
    // latches ToolRegistry's cancel flag, and only newTurn() ever cleared it -- the
    // skill path never calls newTurn(), so the first probe hit cancelled the whole run
    // before round 1 with no output at all. ESC during the run still works: it sets
    // the same flag again, and the runner probes it between steps.
    tools.resetCancel();
    if (agent->ensureStorage) agent->ensureStorage();

    joinSkillThread();
    agentThreadRunning_.store(true);
    skillThread_ = std::thread([this, name, topic, persisted]() {
        runSkillBlocking(name, topic, persisted);
        agentThreadRunning_.store(false);
        if (onTurnComplete) onTurnComplete();
    });
}

std::vector<bg_task::Snapshot> TuiApp::taskSnapshots() const {
    return taskRegistry_.snapshots();
}

std::vector<std::string> TuiApp::installedSkills() const {
    return SkillRunner::listSkills(skillsDir_);
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
    // Detached children: kill them and join their monitor thread. A background build
    // that outlives the TUI would keep writing to a file nobody reads. The Job Object
    // (KILL_ON_JOB_CLOSE) is the second line of defence for a hard crash.
    taskRegistry_.shutdown();
    // MCP last: killing servers is only safe once no callTool can be in flight,
    // and every server's worker thread is joined here (never detached).
    if (mcpManager_) mcpManager_->shutdownAll();
    tools.setProcessRunner(nullptr); // detach before main deletes the runner
    delete agent;
    agent = nullptr;
    // Explicitly close the DB and checkpoint the WAL on exit (bug fix).
    storage.closeDatabase();
    // Drop the host-side status record so no zombie "working" stays behind.
    terminal_status::shutdown();
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
    // Ctrl-C / window close: leave `error` as the last record instead of a
    // silent clear, so an interrupted run is visible in the host's list.
    terminal_status::abandon("interrupted");
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

void TuiApp::saveFullConfig(const AppConfig& next) {
    // One source of truth: the dialog edits a copy of AppConfig and hands it back,
    // so a key that exists in AppConfig cannot be silently dropped by the panel.
    config.apiKey = next.apiKey;
    config.baseUrl = next.baseUrl;
    config.model = next.model;
    config.maxTokens = next.maxTokens;
    config.temperature = next.temperature;
    config.reasoningEffort = next.reasoningEffort;
    config.contextWindow = next.contextWindow;
    config.cppCompilerPath = next.cppCompilerPath;
    config.pythonPath = next.pythonPath;
    config.clangFormatPath = next.clangFormatPath;
    config.snapshotsMaxWorkspaceGb = next.snapshotsMaxWorkspaceGb;
    config.snapshotsKeepDays = next.snapshotsKeepDays;
    config.oscStatus = next.oscStatus;
    config.approvalMode = next.approvalMode;
    config.approvalGrants = next.approvalGrants;
    config.themeName = next.themeName;
    config.loaded = true;
    config.loadError.clear();

    if (!saveConfig(config)) {
        config.loadError = "Failed to save config file!";
    }

    client.setConfig(config);
    // Tool-table writes must not race the agent thread: ToolRegistry's maps are a
    // bare unordered_map and the Agent iterates them in buildChatRequest() ->
    // getFilteredToolDefinitions(). While a turn (or a skill -- it sets the same
    // busy flag) is running, both the rebuild and the Agent setters are deferred
    // to the next turn boundary (runAgentTurnLoop). This is the pre-existing F2
    // race the MCP design surfaced; it is fixed here rather than worked around.
    if (isBusy()) {
        toolsReloadRequested_.store(true);
        debugLogf("[TuiApp] config saved during a running turn: tool reload deferred");
    } else {
        setupTools(); // re-register tools with new workspace path
        applyConfigToAgent();
    }
    config.loadError.clear();
}

void TuiApp::newChat() {
    reinitConversation(false);
}

void TuiApp::injectFixedBlocks() {
    if (!agent) return;
    const std::string ws = getWorkspaceDir();
    const std::string projv = getProjvDir();
    // P1: fixed for the session. Written even when every field is empty (the
    // builder always emits every line), because the fixed prefix must keep a
    // constant message count -- a varying count moves the compaction summary and
    // the per-turn block, and breaks the request prefix cache.
    const std::string env = ctx::buildEnvironment(ws, projv, ws, config.cppCompilerPath,
                                                  config.pythonPath, config.clangFormatPath);
    agent->addPersistedMessage(Message::System(env));
    // P2: directory tree + AGENTS.md, one message, same reason.
    const std::string project = buildProjectBlock(ws);
    agent->addPersistedMessage(Message::System(project));
    debugLogf("[TuiApp] fixed blocks: environment %s, project %s", ctx::describe(env).c_str(),
              ctx::describe(project).c_str());
}

void TuiApp::refreshEnvironmentBlock() {
    if (!agent) return;
    const std::string ws = getWorkspaceDir();
    const std::string env = ctx::buildEnvironment(ws, getProjvDir(), ws, config.cppCompilerPath,
                                                  config.pythonPath, config.clangFormatPath);
    if (agent->replaceEnvironmentBlock(env))
        debugLogf("[TuiApp] environment block refreshed (%s)", ctx::describe(env).c_str());
}

void TuiApp::refreshCapabilitiesBlock() {
    if (!agent) return;
    ctx::CapabilityInputs in;
    in.tools = tools.getToolDefinitions();
    in.allowed = agent->getAllowedTools();
    in.skills = SkillRunner::listSkills(skillsDir_);
    in.skillsDir = skillsDir_;
    if (mcpManager_) {
        for (const auto& purpose : mcpManager_->serverPurposes())
            in.mcp.push_back(ctx::McpServerInfo{purpose.name, purpose.description, purpose.tools});
    }
    std::string block = ctx::buildCapabilities(in);
    // P3 is never persisted, so this log line is its only observable output.
    debugLogf("[TuiApp] capabilities block: %s", ctx::describe(block).c_str());
    agent->setCapabilitiesBlock(std::move(block));
}

void TuiApp::applyConfigToAgent() {
    if (!agent) return;
    // The toolchain paths are part of P1 now, so refresh that message: it is
    // persisted, and a saved config must not leave a stale path until the next F3.
    refreshEnvironmentBlock();
    agent->setRequestParams(config.maxTokens, config.temperature);
    agent->setReasoningEffort(config.reasoningEffort);
    agent->setApprovalMode(config.approvalMode);
    agent->setPersistentGrants(config.approvalGrants);
    agent->setModel(config.model);
    agent->setContextWindow(0); // reset auto-detect on model switch
}

void TuiApp::setMcpEnabled(const std::string& name, bool enabled) {
    if (!mcpManager_) return;
    if (isBusy()) {
        // Idle-only by design: start/stop touches the process tree and the tool
        // table, and neither may interleave with a running turn.
        debugLogWarnf("[TuiApp] setMcpEnabled('%s') refused: a turn is running", name.c_str());
        return;
    }
    mcpManager_->setEnabled(name, enabled);
    // The capability map is rebuilt at the next turn boundary (it names the tools
    // that are actually registered), so nothing to inject here.
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
        std::string sp = activePromptContent();
        agent->setSystemPrompt(sp);
        agent->addPersistedMessage(Message::System(sp));
    }
    injectFixedBlocks();
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
        debugLogWarn("[TuiApp] Failed to open, reverting to previous database");
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
    std::string content = activePromptContent();
    agent->replaceSystemPrompt(content);
}

std::string TuiApp::activePromptContent() const {
    if (activePromptIndex_ < 0 || activePromptIndex_ >= static_cast<int>(promptFiles_.size())) {
        // Nothing to load: every prompt file was deleted and seeding failed. An
        // empty system prompt makes the agent fall back to "all tools allowed",
        // which is loud in the logs but never crashes on an out-of-range index.
        debugLog("[TuiApp] No role prompt available -- empty system prompt");
        return {};
    }
    return loadPromptFile(promptFiles_[static_cast<size_t>(activePromptIndex_)]);
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
