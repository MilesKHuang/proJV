// proJV -- SessionRuntime implementation (design 08).
//
// Almost every body here is a move, not a rewrite: the code lived in
// src/tui/app_tui.cpp and worked. Two things changed on the way in:
//   * `chatHistory.push_back(bubble)` became `notice(...)` -- an event plus a
//     callback, which is exactly what a system line in the chat always was;
//   * `agent->startTurn(...) + launchAgentThread()` became `pendingTurn_ + run()`,
//     so a headless caller and the TUI drive the same code.
#include "core/session_runtime.h"

#include "core/config.h"
#include "core/context_assembler.h"
#include "core/image_input.h"
#include "core/prompts.h"
#include "core/project_context.h"
#include "core/skill_trigger.h"
#include "core/snapshot.h"
#include "core/terminal_status.h"
#include "tools/file_tool.h"
#include "tools/format_tool.h"
#include "tools/json_utils.h"
#include "tools/md_file_tool.h"
#include "tools/shell_tool.h"
#include "tools/web_tools.h"
#include "debug_log.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <utility>

namespace fs = std::filesystem;

SessionRuntime::SessionRuntime() = default;

SessionRuntime::~SessionRuntime() {
    shutdown();
}

// ---- events out ---------------------------------------------------------

void SessionRuntime::emit(events::Event e) {
    if (onEvent) onEvent(e);
}

void SessionRuntime::notice(const std::string& text) {
    events::Event e(events::Kind::Notice, nextSeq());
    e.text = text;
    emit(e);
    if (onNotice) onNotice(text);
}

// ---- tools --------------------------------------------------------------

void SessionRuntime::setWorkspace(const std::string& dir) {
    // Stored, not applied: initialize() builds paths_ from it, so calling this
    // after initialize() would leave the tools on the old root. That is why the
    // header says "before initialize()".
    workspace_ = dir;
}

void SessionRuntime::setupTools() {
    // Two roots, not one: the workspace is the launch directory (every file tool,
    // the shell CWD and the pytool sandbox root there), and projv_files sits next
    // to the exe. projv_files is writable -- that is what "add a skill" means --
    // except sessions/ and logs/, which are history and stay read-only.
    if (workspace_.empty()) workspace_ = getWorkspaceDir();
    paths_ = ToolPaths::forSession(workspace_, getProjvDir());
    const std::string ws = workspace_;
    if (agent_) agent_->setWorkspacePath(paths_.describe());

    // Snapshots (/undo). Best effort: an oversized workspace or a read-only
    // store turns undo off with a reason, and the rest of the app keeps going.
    // The handle is per workspace: a second pane on the same folder gets the same
    // one (one baseline, one index.db -- that is the sharing the studio needs), a
    // pane on another folder gets its own and cannot see this one's files.
    {
        std::string snapErr;
        snapshotStore_ = snapshot::open(ws, config_.snapshotsKeepDays,
                                        config_.snapshotsMaxWorkspaceGb, snapErr);
        if (!snapshot::isAvailable(snapshotStore_) && !snapErr.empty())
            debugLogWarnf("[Runtime] undo disabled: %s", snapErr.c_str());
    }

    registerShellTool(tools_, paths_, procRunner_, &taskRegistry_);
    registerTaskOutputTool(tools_, &taskRegistry_);
    registerFileTools(tools_, paths_);
    registerFormatTool(tools_, paths_, procRunner_, config_.clangFormatPath);
    registerMdFileTool(tools_, paths_);
    registerEditFileTool(tools_, paths_);
    registerFileSearchTool(tools_, paths_);
    registerSearchTool(tools_, client_);
    registerFetchTool(tools_, client_);

    // run_skill: start an installed skill. The skill does NOT run inside this
    // executor -- it runs as its own phase right after the current turn ends (see
    // runTurnLoop), through the very same function the "/<skill> <topic>" trigger
    // uses. Consequences:
    //   - narration/document are normal assistant messages: bubbles, the agent
    //     dock, the DB and session replay all behave like a user-triggered run;
    //   - the tool_call pairing is never broken, because this executor returns a
    //     plain tool result and the turn stops right after it;
    //   - ESC keeps working (cancel cancels the published SkillRunner).
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
        tools_.registerTool(def, [this](const std::string& args) -> std::string {
            std::string name = extractStringArg(args, "name");
            std::string topic = extractStringArg(args, "topic");
            if (name.empty()) return "Error: Missing 'name' argument";
            if (topic.empty()) return "Error: Missing 'topic' argument";
            if (!modelSupportsTools(config_.model))
                return "Error: model '" + config_.model +
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

            // End the turn after this tool round; runTurnLoop picks the skill up
            // and resumes the conversation when it is done.
            tools_.requestTurnEnd();
            return "[skill] '" + folder +
                   "' started. It runs as its own phase now; its "
                   "narration and final document will be above your next turn. Ending this "
                   "turn loses nothing.";
        });
    }

    // view_image: the model asks to look at images on disk. The tool returns no
    // pixels -- it registers an image round and ends the turn, exactly like
    // run_skill. runTurnLoop then appends a synthetic user message carrying the
    // attachments (the API only accepts images in user messages), so the next turn
    // can actually see them.
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
        tools_.registerTool(def, [this, ws](const std::string& args) -> std::string {
            if (!modelSupportsVision(config_.model))
                return "Error: model '" + config_.model +
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

            tools_.requestTurnEnd();
            return "[image] " + std::to_string(count) +
                   " image(s) queued. They arrive as a user message in the next turn; this "
                   "turn ends here, nothing is lost.";
        });
    }

    pytoolMgr_.emplace(config_.pythonPath, ws);
    pytoolMgr_->scanAndRegister(tools_, procRunner_);

    // Wire the process runner so ToolRegistry::cancelAll() can interrupt a
    // running exec_shell / pytool subprocess (Agent::cancel() -> cancelAll()).
    tools_.setProcessRunner(procRunner_);
}

// ---- initialize ---------------------------------------------------------

bool SessionRuntime::initialize() {
    loadTomlConfig(config_, "");
    client_.setConfig(config_);
    setupTools();
    skillsDir_ = getProjvDir() + "/skills";
    SkillRunner::ensureDefaultSkills(skillsDir_);

    agent_ = new Agent(client_, tools_);
    agent_->setSnapshotStore(snapshotStore_);
    agent_->onTokenUsage = [this](int p, int c) {
        totalPromptTokens_.fetch_add(p, std::memory_order_relaxed);
        totalCompletionTokens_.fetch_add(c, std::memory_order_relaxed);
        events::Event e(events::Kind::Usage, nextSeq());
        e.prompt = p;
        e.completion = c;
        emit(e);
    };
    agent_->onStreamingTick = [this] {
        if (onStreamingTick) onStreamingTick();
    };
    // Mirror the agent loop outward: one event for every front-end, plus the
    // optional host-terminal record the TUI installs through onPhase.
    agent_->setPhaseCallback([this](AgentPhase p, const std::string& msg) {
        const char* name = "idle";
        switch (p) {
        case AgentPhase::Streaming:
            name = "streaming";
            break;
        case AgentPhase::ExecutingTools:
            name = "executing_tools";
            break;
        case AgentPhase::AwaitApproval:
            name = "await_approval";
            break;
        case AgentPhase::Error:
            name = "error";
            break;
        case AgentPhase::Idle:
        default:
            name = "idle";
            break;
        }
        events::Event e(events::Kind::Phase, nextSeq());
        e.phase = name;
        e.text = msg;
        emit(e);
        if (onPhase) onPhase(p, msg);
        if (onStreamingTick) onStreamingTick(); // wake the UI thread
    });
    agent_->setRequestParams(config_.maxTokens, config_.temperature);
    agent_->setReasoningEffort(config_.reasoningEffort);
    agent_->setApprovalMode(config_.approvalMode);
    agent_->setPersistentGrants(config_.approvalGrants);
    // ALWAYS -> write it into config.toml. Core has no config access; the runtime
    // owns the single AppConfig copy, so persistence happens here.
    agent_->onGrantPersist = [this](const std::string& cmd) {
        if (std::find(config_.approvalGrants.begin(), config_.approvalGrants.end(), cmd) ==
            config_.approvalGrants.end())
            config_.approvalGrants.push_back(cmd);
        saveConfig(config_);
        debugLogf("[Runtime] approval grant persisted: %s", cmd.c_str());
    };
    agent_->setWorkspacePath(paths_.describe());
    agent_->setModel(config_.model);
    agent_->setContextWindow(config_.contextWindow);

    // Prompt files from projv_files/prompts/.
    // ensureDefaultPrompts() is the ONLY thing that creates that directory and
    // seeds the default role, so it must run before anything reads the list.
    promptFiles_ = ensureDefaultPrompts();
    // compactor.md is the /compress prompt, not a role. Every other file here is
    // a role, whatever it is called -- no name is reserved.
    promptFiles_.erase(std::remove(promptFiles_.begin(), promptFiles_.end(), "compactor.md"),
                       promptFiles_.end());
    activePromptIndex_ = defaultPromptIndex(promptFiles_);
    agent_->setSystemPrompt(activePromptContent());

    // Sessions dir (lazy-create DB on first message). Without this assignment the
    // directory name is empty: the DB path collapses to "/session_<ts>.db" (the
    // root of the current drive on Windows) and listSessions() iterates "" -- so
    // F4/Open came up blank and a new conversation was never persisted.
    sessionsDir_ = getSessionsDir();
    std::error_code ec;
    fs::create_directories(sessionsDir_, ec);
    if (ec) {
        debugLogWarnf("[Runtime] Failed to create sessions dir '%s': %s", sessionsDir_.c_str(),
                      ec.message().c_str());
    }

    agent_->ensureStorage = [this]() {
        if (storage_.isOpen()) return;
        auto now = std::time(nullptr);
        char tsBuf[64];
        tm local;
#ifdef _MSC_VER
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        strftime(tsBuf, sizeof(tsBuf), "%Y%m%d_%H%M%S", &local);
        const std::string dbPath = sessionsDir_ + "/session_" + tsBuf + ".db";
        if (storage_.createDatabase(dbPath)) {
            agent_->setStorage(&storage_);
            debugLogf("[Runtime] Lazy-created database: %s", dbPath.c_str());
            for (const auto& msg : agent_->getSession().getContextMessages()) {
                if (msg.role == "system") storage_.insertMessage(msg);
            }
        } else {
            debugLogErrf("[Runtime] Failed to lazy-create database: %s", dbPath.c_str());
        }
    };

    // Inject system prompt as first session message: P0 (role), so the fixed
    // prefix is exactly three system messages. The index is what buildChatRequest
    // and applyCompaction rely on to keep P0/P1/P2 at [0, 3) -- with this line
    // missing the prefix shrank to two, and the capability map plus the compaction
    // summary were inserted one slot early.
    agent_->addPersistedMessage(Message::System(activePromptContent()));

    // Fixed prefix: P1 [ENVIRONMENT] + P2 [PROJECT] right after the role prompt.
    // Same call the F3 and "/clear" paths use, so a conversation always starts
    // with the same three system messages.
    injectFixedBlocks();

    // MCP: read projv_files/mcp.json and spawn every enabled + trusted server.
    // Spawning happens on per-server worker threads, so nothing here blocks; tools
    // are registered at the first turn boundary (see runTurnLoop), which is also
    // where the capability map that names them is built.
    mcpManager_ = std::make_unique<mcp::Manager>();
    mcpManager_->setWorkDir(workspace_);
    mcp::ensureConfigFile(mcp::configPath());
    if (!mcpManager_->loadFromJson(mcp::configPath()))
        debugLogWarnf("[Runtime] mcp.json: %s", mcpManager_->configError().c_str());
    mcpManager_->startEnabled();

    return true;
}

// ---- submit: every user line enters here --------------------------------

bool SessionRuntime::submit(const std::string& text) {
    if (!agent_ || text.empty()) return false;

    // Busy: queue the message instead of dropping it (drained when the turn ends).
    if (busy_.load()) {
        pendingQueue_.push_back(text);
        return false;
    }

    // Idle -- the only safe window to add to or erase from the MCP server vector
    // (the render loop walks it every frame). This is what makes "edit mcp.json,
    // then send a message" work without a restart.
    if (mcpManager_) {
        if (mcpManager_->reloadIfChanged(mcp::configPath()))
            debugLogf("[Runtime] mcp.json reloaded before this turn");
        // Handles removed by the reload are unregistered by syncTools at the turn
        // boundary; only then is it safe to drop them from the pool.
        mcpManager_->reapRemoved();
    }

    // Ensure the session DB exists so quick commands (/help etc.) persist.
    if (agent_->ensureStorage) agent_->ensureStorage();

    std::string trimmed = text;
    auto s = trimmed.find_first_not_of(" \t\r\n");
    auto e = trimmed.find_last_not_of(" \t\r\n");
    trimmed = (s == std::string::npos) ? std::string() : trimmed.substr(s, e - s + 1);
    if (trimmed.empty()) return false;

    // /workspace: display only, no turn.
    if (trimmed == "/workspace") {
        notice("[Workspace] " + workspace_);
        return false;
    }

    if (trimmed == "/compress") {
        agent_->addPersistedMessage(Message::User("/compress"));
        agent_->requestCompact();
        return true; // run() sees compactRequested_ and compresses
    }

    // /<skill> <topic>: direct skill trigger (rules live in core/skill_trigger,
    // unit-tested). An unknown token resolves to Kind::None and falls through as
    // an ordinary message, so /usr/bin paths, "//" comments and natural language
    // are never intercepted. "/skill list" stays the safe listing entry point.
    // This branch runs BEFORE the vision "@path" parsing below, so
    // "/monica D:\dir @C:\a.png" hands the whole line to the skill engine.
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
            notice(msg);
            return false;
        }

        if (trig.kind == skill_trigger::Result::Kind::Trigger) {
            if (trig.topic.empty()) {
                notice("[skill] usage: /" + trig.name + " <topic>");
                return false;
            }
            // Skills carry no image input yet: say so instead of silently
            // dropping the attachments.
            if (trimmed.find('@') != std::string::npos)
                notice("[image] images ignored: skills do not carry image input yet.");
            pendingSkillRun_ = PendingSkillRun{trig.name, trig.topic, trimmed};
            return true;
        }
    }

    // "/clear" is handled here rather than in Agent: wiping the session must also
    // restore the fixed system prompt and the workspace context, and the core has
    // no access to the prompt files. Same path as F3, except the session database
    // (and the visible history) stay as they are.
    if (trimmed == "/clear") {
        reinitConversation(true);
        agent_->addPersistedMessage(Message::System(
            "[clear] context cleared. System prompt, directory tree and AGENTS.md were "
            "reloaded; the visible history is kept, the model starts fresh."));
        notice("[clear] context cleared; prompt and workspace context reloaded.");
        return false;
    }

    // Other quick commands handled by the backend (/help /undo /restore).
    if (agent_->handleQuickCommand(trimmed)) return false;

    // /wish: start, inspect or cancel the wish mode. Starting it also starts a
    // turn -- a wish that only sets a flag would sit there doing nothing.
    if (trimmed == "/wish" || trimmed.rfind("/wish ", 0) == 0) {
        std::string arg = trimmed.size() > 6 ? trimmed.substr(6) : std::string();
        const size_t b = arg.find_first_not_of(" \t");
        arg = (b == std::string::npos) ? std::string() : arg.substr(b);

        if (arg.empty()) {
            const Wish w = agent_->wishSnapshot();
            std::string msg;
            if (!w.active) {
                msg = "[wish] not active. Usage: /wish <what you want built>";
            } else {
                msg = "[wish] active: " + w.objective + "\n" + std::to_string(w.passes) +
                      " pass(es), " + std::to_string(w.decisions) + " decision(s), " +
                      (agent_->hasUnfinishedTodo() ? "TODO still open" : "TODO clear") +
                      ".\n/wish cancel stops it; the TODO list is kept.";
            }
            notice(msg);
            return false;
        }
        if (arg == "cancel") {
            agent_->cancelWish();
            notice("[wish] cancelled. The TODO list and the recorded decisions are kept.");
            return false;
        }
        agent_->startWish(arg);
        notice("[wish] started: " + arg +
               "\nThe model decides the open questions itself and keeps working until its "
               "TODO list is empty. /wish cancel stops it.");
        pendingTurn_ = PendingTurn{arg, {}};
        return true;
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
            if (snaps.empty()) {
                msg = "[bg] no background tasks in this session.";
            } else {
                msg = "[bg] tasks:";
                for (const auto& t : snaps)
                    msg += "\n- bg#" + std::to_string(t.id) + " " + bg_task::stateLabel(t.state) +
                           (t.exitCode >= 0 ? " (exit " + std::to_string(t.exitCode) + ")" : "") +
                           " -- " + t.command;
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
        notice(msg);
        return false;
    }

    // /approval: what the policy is and what has been remembered. Without this the
    // persisted grants are invisible, and an invisible allow-list is a bad one.
    if (trimmed == "/approval" || trimmed.rfind("/approval ", 0) == 0) {
        std::string arg = trimmed.size() > 9 ? trimmed.substr(9) : std::string();
        const size_t b = arg.find_first_not_of(" \t");
        arg = (b == std::string::npos) ? std::string() : arg.substr(b);
        std::string msg;
        if (arg == "clear") {
            agent_->clearGrants();
            config_.approvalGrants.clear();
            saveConfig(config_);
            msg = "[approval] remembered commands cleared.";
        } else if (!arg.empty() && arg != "list") {
            msg = "[approval] usage: /approval, /approval clear";
        } else {
            msg = "[approval] mode: " + approvalModeName(agent_->approvalMode()) +
                  "\n  strict   -- every delete/overwrite asks, no ALWAYS\n"
                  "  balanced -- destructive asks, build output runs (default)\n"
                  "  loose    -- only format/partition is refused\n";
            const auto grants = agent_->grants();
            if (grants.empty()) {
                msg += "remembered: (none)";
            } else {
                msg += "remembered (" + std::to_string(grants.size()) + "):";
                for (const auto& g : grants)
                    msg += "\n- " + g;
                msg += "\n/approval clear forgets them all.";
            }
        }
        notice(msg);
        return false;
    }

    // "@<path>" image markers (vision). Runs after the skill branch, so a
    // "/monica ... @C:\a.png" line was already handed to the skill engine.
    std::string turnText = trimmed;
    std::vector<ImageAttachment> images;
    {
        auto parsed = image_input::parseMarkers(trimmed, workspace_);
        if (!parsed.ok()) {
            notice(parsed.error + "\n(message not sent)");
            return false;
        }
        if (!parsed.images.empty()) {
            if (!modelSupportsVision(config_.model)) {
                notice("[image] model '" + config_.model +
                       "' does not accept image input. Switch to a Flash model (F10).\n"
                       "(message not sent)");
                return false;
            }
            images = std::move(parsed.images);
            turnText = parsed.text;
            // Markers-only message: give the turn something to answer.
            if (turnText.empty()) turnText = "Describe the attached image(s).";
        }
    }

    pendingTurn_ = PendingTurn{turnText, std::move(images)};
    return true;
}

// ---- run: the blocking body --------------------------------------------

void SessionRuntime::run() {
    if (busy_.exchange(true)) return; // one runner at a time
    doWork();
    busy_.store(false);
}

void SessionRuntime::startAsync() {
    if (busy_.exchange(true)) return;
    joinAgentThread();
    agentLatch_.reset();
    agentThread_ = std::thread([this] {
        doWork();
        busy_.store(false);
        if (onTurnComplete) onTurnComplete();
        // Last statement of the worker: an exit waiting on this latch now knows the
        // turn really finished instead of guessing with a timeout.
        agentLatch_.signal();
    });
}

void SessionRuntime::doWork() {
    if (!agent_) return;

    // A user-triggered skill is its own phase: no model turn first, exactly like
    // the "/<skill> <topic>" line always meant.
    std::optional<PendingSkillRun> skillRun;
    if (pendingSkillRun_) {
        skillRun = std::move(pendingSkillRun_);
        pendingSkillRun_.reset();
    }
    if (skillRun) {
        // A user-invoked skill starts un-cancelled. Agent::cancel() (ESC, F3, F4,
        // /clear) latches ToolRegistry's cancel flag and only newTurn() ever cleared
        // it -- a skill phase never calls newTurn(), so without this the first cancel
        // probe hit kills the whole run before round 1 with no output at all. The
        // guard lived in TuiApp::startSkill before the move; both trigger paths (the
        // run_skill tool and a typed "/<skill> <topic>") now meet here, so it lives
        // here.
        tools_.resetCancel();
        events::Event start(events::Kind::TurnStart, nextSeq());
        start.turn = "skill:" + skillRun->name;
        start.model = config_.model;
        start.session = storage_.sessionId();
        emit(start);
        const bool ok = runSkillBlocking(skillRun->name, skillRun->topic, skillRun->rawInput);
        events::Event end(events::Kind::TurnEnd, nextSeq());
        end.reason = ok ? "skill_done" : "skill_cancelled";
        emit(end);
        return;
    }

    if (pendingTurn_) {
        PendingTurn t = std::move(*pendingTurn_);
        pendingTurn_.reset();
        agent_->startTurn(t.text, std::move(t.images));
    }
    runTurnLoop();
}

void SessionRuntime::runTurnLoop() {
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
            debugLogf("[Runtime] applying deferred tool reload (config saved mid-turn)");
            setupTools();
            applyConfigToAgent();
        }
        if (mcpManager_) {
            const mcp::Manager::SyncResult res = mcpManager_->syncTools(tools_);
            if (!res.added.empty()) agent_->addAutoTools(res.added);
            if (!res.removed.empty()) agent_->removeAutoTools(res.removed);
        }
        // P3 after the registry is settled: the capability map must name exactly
        // the tools the request will carry.
        refreshCapabilitiesBlock();

        agent_->newTurn();
        {
            events::Event start(events::Kind::TurnStart, nextSeq());
            start.turn = std::to_string(++turnSeq_);
            start.model = config_.model;
            start.session = storage_.sessionId();
            emit(start);
        }
        agent_->run();
        {
            // A failed turn must be visible on the stream even when the agent
            // settled on Idle: the HTTP-retry path only leaves a persisted message.
            std::string turnReason = "idle";
            if (agent_->turnFailed()) {
                turnReason = "error";
                events::Event err(events::Kind::Error, nextSeq());
                err.text = agent_->turnError();
                emit(err);
            } else if (tools_.isCancelled()) {
                turnReason = "cancelled";
            }
            events::Event end(events::Kind::TurnEnd, nextSeq());
            end.reason = turnReason;
            emit(end);
        }
        // ESC during the model turn: stop. newTurn() resets the cancel flags,
        // so this check has to happen before the next iteration starts.
        if (tools_.isCancelled()) break;

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
            agent_->addPersistedMessage(msg);
            if (tools_.isCancelled()) break;
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
            if (tools_.isCancelled()) break;

            // One-shot System message for the follow-up turn. A skill's output is
            // raw material, not a verdict: pipelines like avenger -> steve -> musk
            // need the main agent to review the produced document and say what is
            // weak before moving on, so the wrap-up is NOT capped to a few lines.
            agent_->setTurnNudge(
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
                    const bg_task::Snapshot* snap = nullptr;
                    for (const auto& cand : snaps)
                        if (cand.id == done[i]) snap = &cand;
                    msg += "bg#" + std::to_string(done[i]);
                    if (snap)
                        msg += " " + std::string(bg_task::stateLabel(snap->state)) + " (exit " +
                               std::to_string(snap->exitCode) + "), " +
                               std::to_string(snap->outputBytes) + " bytes: " + snap->command;
                    msg += "\nRead it with task_output(" + std::to_string(done[i]) + ").";
                    events::Event e(events::Kind::Task, nextSeq());
                    e.id = "bg#" + std::to_string(done[i]);
                    if (snap) {
                        e.activity = bg_task::stateLabel(snap->state);
                        e.exitCode = snap->exitCode;
                        e.hasExit = true;
                        e.bytes = static_cast<int>(snap->outputBytes);
                        e.cmd = snap->command;
                    }
                    emit(e);
                }
                notice(msg);
                agent_->addPersistedMessage(Message::System(msg));
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
            const Wish w = agent_->endWishPass();
            if (w.active) {
                const std::string stop = agent_->wishStopReason(w);
                if (stop.empty()) continue;
                std::string msg = "**Wish stopped** (" + stop + ")\n";
                const auto decisions = agent_->wishDecisions();
                if (!decisions.empty()) {
                    msg += "\nDecisions taken for you:\n";
                    for (const auto& d : decisions)
                        msg += "- " + d + "\n";
                }
                const std::string todo = agent_->unfinishedTodoText();
                if (!todo.empty()) msg += "\nStill open:\n" + todo;
                notice(msg);
                agent_->cancelWish();
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

// ---- skill phase --------------------------------------------------------

bool SessionRuntime::runSkillBlocking(const std::string& name, const std::string& topic,
                                      const std::string& rawInput) {
    if (!agent_) return false;
    if (agent_->ensureStorage) agent_->ensureStorage();

    // Persist the exact user line so history replays match what was typed.
    // Empty == agent-triggered (run_skill): the user never typed anything, so do
    // not invent a user bubble -- the "[Role] ..." narration is the evidence.
    if (!rawInput.empty()) agent_->addPersistedMessage(Message::User(rawInput));
    // One id per run, stamped on every message the run writes: that is what lets
    // the chat fold a whole debate into one block, and because it is persisted a
    // replay regroups exactly as the live session did. Wall-clock based rather
    // than an in-process counter -- a counter restarts at zero and would collide
    // with the ids already sitting in the database.
    const std::string skillRunId =
        "skill-" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::system_clock::now().time_since_epoch())
                                      .count());
    {
        std::lock_guard<std::mutex> lk(skillMutex_);
        skillStatus_ = "Preparing skill...";
    }
    // The main loop is Idle during a skill phase, so the state has to be
    // reported from here or the host would show "idle" while a debate runs.
    if (terminalStatusEnabled_) terminal_status::working("skill: " + name);
    if (onStreamingTick) onStreamingTick();

    SkillRunner::Callbacks cbs;
    int events = 0;
    cbs.onEvent = [this, &events, skillRunId](const std::string& role, const std::string& kind,
                                              const std::string& text) {
        ++events;
        std::string content;
        if (kind == "vote")
            content = "**[" + role + " vote]** " + text;
        else if (kind == "document")
            content = text;
        else
            content = "**[" + role + "]** " + text; // message / system
        if (agent_) {
            Message m = Message::Assistant(content);
            m.skillRunId = skillRunId;
            agent_->addPersistedMessage(m);
        }

        events::Event e(events::Kind::SkillEvent, nextSeq());
        e.role = role;
        e.sub = kind;
        e.text = text;
        emit(e);
    };
    cbs.onProgress = [this](const std::string& st) {
        {
            std::lock_guard<std::mutex> lk(skillMutex_);
            skillStatus_ = st;
        }
        if (onStreamingTick) onStreamingTick();
    };

    auto runner =
        std::make_shared<SkillRunner>(config_, &tools_, cbs, buildSharedContext(), skillsDir_);
    runner->setCancelProbe([this]() { return tools_.isCancelled(); });

    SkillRunner* r = runner.get();
    {
        // Publish under the lock so the dock (skillAgentStatuses) and ESC (cancel)
        // can reach this runner while it runs.
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
        if (agent_) agent_->addPersistedMessage(Message::System(msg));
    } else if (events == 0) {
        debugLogWarnf("[skill] %s produced no event (topic: %.120s)", name.c_str(), topic.c_str());
        const std::string msg =
            "[skill] " + name + " finished without producing a single message (topic: " + topic +
            ").\nCommon causes: the topic is not something the skill can act on, the agent "
            "budget ran out before the first round, or the API rejected the first request. "
            "Check projv_files/logs/proJV-<date>.log and try a narrower topic.";
        if (agent_) agent_->addPersistedMessage(Message::System(msg));
    }

    {
        std::lock_guard<std::mutex> lk(skillMutex_);
        skillRunner_.reset();
        skillStatus_.clear();
    }
    if (terminalStatusEnabled_) terminal_status::done("skill: " + name + " finished");
    if (onTurnComplete) onTurnComplete();
    return !tools_.isCancelled();
}

// ---- front-end actions --------------------------------------------------

void SessionRuntime::startSkill(const std::string& name, const std::string& topic,
                                const std::string& rawInput) {
    // Identical to the user typing "/<skill> <topic>": one code path, so the
    // history row, the bubbles and the dock behave the same either way.
    const std::string line = rawInput.empty() ? ("/" + name + " " + topic) : rawInput;
    if (submit(line)) startAsync();
}

void SessionRuntime::cancel() {
    std::shared_ptr<SkillRunner> runner;
    {
        std::lock_guard<std::mutex> lk(skillMutex_);
        pendingQueue_.clear();
        pendingSkill_.reset(); // drop a queued-but-not-started skill
        pendingImages_.reset();
        pendingSkillRun_.reset();
        turnRequest_.release();
        // Take a reference, cancel OUTSIDE the lock: cancel() reaches into every
        // sub-agent's HTTP call, and holding skillMutex_ while doing that is how an
        // ESC on the UI thread ended up waiting behind a status publish.
        runner = skillRunner_;
    }
    if (runner) runner->cancel();
    pendingTurn_.reset();
    if (agent_) agent_->cancel();
}

size_t SessionRuntime::pendingCount() const {
    std::lock_guard<std::mutex> lk(skillMutex_);
    return pendingQueue_.size();
}

void SessionRuntime::drainPendingQueue() {
    if (busy_.load()) return;
    std::string next;
    {
        std::lock_guard<std::mutex> lk(skillMutex_);
        if (pendingQueue_.empty()) return;
        next = pendingQueue_.front();
        pendingQueue_.erase(pendingQueue_.begin());
    }
    if (submit(next)) startAsync();
}

void SessionRuntime::approveTool(int action) {
    if (agent_) agent_->approveTool(action);
}

std::vector<bg_task::Snapshot> SessionRuntime::taskSnapshots() const {
    return taskRegistry_.snapshots();
}

std::vector<std::string> SessionRuntime::installedSkills() const {
    return SkillRunner::listSkills(skillsDir_);
}

std::string SessionRuntime::skillStatus() const {
    std::lock_guard<std::mutex> lk(skillMutex_);
    return skillStatus_;
}

std::vector<SkillRunner::AgentSnapshot> SessionRuntime::skillAgentStatuses() const {
    std::lock_guard<std::mutex> lk(skillMutex_);
    if (!skillRunner_) return {};
    return skillRunner_->agentStatuses();
}

// The main session rendered as reference material for skill agents.
std::string SessionRuntime::buildSharedContext() {
    if (!agent_) return std::string();
    // A digest, not the transcript. The full conversation used to be pasted into
    // every sub-agent's prompt -- in a long session that is hundreds of thousands of
    // tokens per agent. The digest reuses the compactor (same prompt, same
    // overview + todo + slice input) and never touches the session.
    const std::string digest = agent_->summarizeConversationForSharing();
    if (digest.empty()) return std::string();
    return "[SHARED SESSION CONTEXT - a summary of the main agent's session so far, not the raw "
           "log. Reference material only; use it to resolve vague references.]\n\n" +
           digest;
}

// ---- fixed context blocks ----------------------------------------------

void SessionRuntime::injectFixedBlocks() {
    if (!agent_) return;
    const std::string ws = workspace_;
    const std::string projv = getProjvDir();
    // P1: fixed for the session. Written even when every field is empty (the
    // builder always emits every line), because the fixed prefix must keep a
    // constant message count -- a varying count moves the compaction summary and
    // the per-turn block, and breaks the request prefix cache.
    const std::string env = ctx::buildEnvironment(ws, projv, ws, config_.cppCompilerPath,
                                                  config_.pythonPath, config_.clangFormatPath);
    agent_->addPersistedMessage(Message::System(env));
    // P2: directory tree + AGENTS.md, one message, same reason.
    const std::string project = buildProjectBlock(ws);
    agent_->addPersistedMessage(Message::System(project));
    debugLogf("[Runtime] fixed blocks: environment %s, project %s", ctx::describe(env).c_str(),
              ctx::describe(project).c_str());
}

void SessionRuntime::refreshEnvironmentBlock() {
    if (!agent_) return;
    const std::string ws = workspace_;
    const std::string env = ctx::buildEnvironment(ws, getProjvDir(), ws, config_.cppCompilerPath,
                                                  config_.pythonPath, config_.clangFormatPath);
    if (agent_->replaceEnvironmentBlock(env))
        debugLogf("[Runtime] environment block refreshed (%s)", ctx::describe(env).c_str());
}

void SessionRuntime::refreshCapabilitiesBlock() {
    if (!agent_) return;
    ctx::CapabilityInputs in;
    in.tools = tools_.getToolDefinitions();
    in.allowed = agent_->getAllowedTools();
    in.skills = SkillRunner::listSkills(skillsDir_);
    in.skillsDir = skillsDir_;
    if (mcpManager_) {
        for (const auto& purpose : mcpManager_->serverPurposes())
            in.mcp.push_back(ctx::McpServerInfo{purpose.name, purpose.description, purpose.tools});
    }
    std::string block = ctx::buildCapabilities(in);
    // P3 is never persisted, so this log line is its only observable output.
    debugLogf("[Runtime] capabilities block: %s", ctx::describe(block).c_str());
    agent_->setCapabilitiesBlock(std::move(block));
}

void SessionRuntime::applyConfigToAgent() {
    if (!agent_) return;
    // The toolchain paths are part of P1 now, so refresh that message: it is
    // persisted, and a saved config must not leave a stale path until the next F3.
    refreshEnvironmentBlock();
    agent_->setRequestParams(config_.maxTokens, config_.temperature);
    agent_->setReasoningEffort(config_.reasoningEffort);
    agent_->setApprovalMode(config_.approvalMode);
    agent_->setPersistentGrants(config_.approvalGrants);
    agent_->setModel(config_.model);
    agent_->setContextWindow(0); // reset auto-detect on model switch
}

// ---- session lifecycle --------------------------------------------------

void SessionRuntime::newChat() {
    reinitConversation(false);
}

void SessionRuntime::reinitConversation(bool keepStorage) {
    if (!agent_) return;
    agent_->cancel();
    if (!keepStorage) {
        if (storage_.isOpen()) storage_.closeDatabase();
        agent_->setStorage(nullptr);
        totalPromptTokens_.store(0, std::memory_order_relaxed);
        totalCompletionTokens_.store(0, std::memory_order_relaxed);
    }
    agent_->clearSession();
    // System prompt first: buildChatRequest() inserts its dynamic system block
    // right after the fixed system messages, so the session must start with one.
    {
        std::string sp = activePromptContent();
        agent_->setSystemPrompt(sp);
        agent_->addPersistedMessage(Message::System(sp));
    }
    injectFixedBlocks();
    // A front-end redraws from the database; "/clear" keeps the rows, F3 starts a
    // new one, and either way the view is rebuilt from scratch.
    if (onSessionReset) onSessionReset();
}

bool SessionRuntime::useSessionFile(const std::string& dbPath) {
    if (!agent_ || dbPath.empty()) return false;

    std::error_code ec;
    if (fs::is_regular_file(dbPath, ec)) {
        switchToDialog(dbPath); // existing session: open it and load its history
        return true;
    }
    // A file that is only here because somebody typed the flag wrong would send
    // every turn into a session nobody can find again.
    fs::path parent = fs::path(dbPath).parent_path();
    if (!parent.empty()) fs::create_directories(parent, ec);

    const std::string oldPath = storage_.currentPath();
    agent_->cancel();
    agent_->clearSession();
    storage_.closeDatabase();
    if (!storage_.createDatabase(dbPath)) {
        debugLogWarnf("[Runtime] cannot create the session database '%s'", dbPath.c_str());
        if (!oldPath.empty()) storage_.openDatabase(oldPath);
        agent_->setStorage(&storage_);
        return false;
    }
    agent_->setStorage(&storage_);
    debugLogInfof("[Runtime] session database: %s", dbPath.c_str());
    if (onSessionReset) onSessionReset();
    return true;
}

void SessionRuntime::switchToDialog(const std::string& dbPath) {
    if (!agent_) return;
    agent_->cancel();

    const std::string oldPath = storage_.currentPath();
    agent_->clearSession();

    storage_.closeDatabase();
    if (!storage_.openDatabase(dbPath)) {
        debugLogWarn("[Runtime] Failed to open, reverting to previous database");
        if (!storage_.openDatabase(oldPath)) {
            storage_.createDatabase(sessionsDir_ + "/recovery.db");
        }
    }
    agent_->setStorage(&storage_);
    try {
        agent_->loadFromStorage();
    } catch (const std::exception& e) {
        debugLogf("[Runtime] loadFromStorage exception: %s", e.what());
    }
    if (onSessionReset) onSessionReset();
}

std::vector<std::string> SessionRuntime::listSessions() const {
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

void SessionRuntime::switchPrompt(int index) {
    if (!agent_ || busy_.load()) return; // only while idle
    if (index < 0 || index >= static_cast<int>(promptFiles_.size())) return;
    if (index == activePromptIndex_) return;
    activePromptIndex_ = index;
    agent_->replaceSystemPrompt(activePromptContent());
}

std::string SessionRuntime::activePromptContent() const {
    if (activePromptIndex_ < 0 || activePromptIndex_ >= static_cast<int>(promptFiles_.size())) {
        // Nothing to load: every prompt file was deleted and seeding failed. An
        // empty system prompt makes the agent fall back to "all tools allowed",
        // which is loud in the logs but never crashes on an out-of-range index.
        debugLog("[Runtime] No role prompt available -- empty system prompt");
        return {};
    }
    return loadPromptFile(promptFiles_[static_cast<size_t>(activePromptIndex_)]);
}

void SessionRuntime::setModel(const std::string& model) {
    config_.model = model;
    if (agent_) {
        agent_->setModel(model);
        agent_->setContextWindow(0); // reset auto-detect on model switch
    }
    saveConfig(config_);
}

void SessionRuntime::setMcpEnabled(const std::string& name, bool enabled) {
    if (!mcpManager_) return;
    if (busy_.load()) {
        // Idle-only by design: start/stop touches the process tree and the tool
        // table, and neither may interleave with a running turn.
        debugLogWarnf("[Runtime] setMcpEnabled('%s') refused: a turn is running", name.c_str());
        return;
    }
    mcpManager_->setEnabled(name, enabled);
    // The capability map is rebuilt at the next turn boundary (it names the tools
    // that are actually registered), so nothing to inject here.
}

void SessionRuntime::saveApiKeyAndConnect(const std::string& apiKey) {
    config_.apiKey = apiKey;
    config_.loaded = true;
    config_.loadError.clear();
    if (!saveApiKey(config_.apiKey)) config_.loadError = "Failed to save API key";
    client_.setConfig(config_);
    config_.loadError.clear();
}

void SessionRuntime::saveFullConfig(const AppConfig& next) {
    // One source of truth: the dialog edits a copy of AppConfig and hands it back,
    // so a key that exists in AppConfig cannot be silently dropped by the panel.
    const std::string previousPython = config_.pythonPath;
    config_.apiKey = next.apiKey;
    config_.baseUrl = next.baseUrl;
    config_.model = next.model;
    config_.maxTokens = next.maxTokens;
    config_.temperature = next.temperature;
    config_.reasoningEffort = next.reasoningEffort;
    config_.contextWindow = next.contextWindow;
    config_.cppCompilerPath = next.cppCompilerPath;
    config_.pythonPath = next.pythonPath;
    config_.clangFormatPath = next.clangFormatPath;
    config_.snapshotsMaxWorkspaceGb = next.snapshotsMaxWorkspaceGb;
    config_.snapshotsKeepDays = next.snapshotsKeepDays;
    config_.oscStatus = next.oscStatus;
    config_.approvalMode = next.approvalMode;
    config_.approvalGrants = next.approvalGrants;
    config_.themeName = next.themeName;
    config_.loaded = true;
    config_.loadError.clear();

    if (!saveConfig(config_)) config_.loadError = "Failed to save config file!";

    client_.setConfig(config_);
    // The auto-detection cache is keyed on nothing; a changed python path must
    // invalidate it, or the new setting is ignored for the rest of the process.
    if (previousPython != config_.pythonPath) PythonToolManager::resetPythonCache();
    // Tool-table writes must not race the agent thread: ToolRegistry's maps are a
    // bare unordered_map and the Agent iterates them in buildChatRequest() ->
    // getFilteredToolDefinitions(). While a turn (or a skill -- it sets the same
    // busy flag) is running, both the rebuild and the Agent setters are deferred
    // to the next turn boundary (runTurnLoop). This is the pre-existing F2 race
    // the MCP design surfaced; it is fixed here rather than worked around.
    if (busy_.load()) {
        toolsReloadRequested_.store(true);
        debugLogf("[Runtime] config saved during a running turn: tool reload deferred");
    } else {
        setupTools(); // re-register tools with new workspace path
        applyConfigToAgent();
    }
    config_.loadError.clear();
}

void SessionRuntime::refreshModels() {
    if (modelsLoading_.exchange(true)) return; // already loading
    joinModelsThread();
    modelsThread_ = std::thread([this] {
        std::string err;
        auto models = client_.fetchModels(&err);
        if (!models.empty()) {
            std::lock_guard<std::mutex> lk(modelsMutex_);
            availableModels_ = std::move(models);
        }
        modelsLoading_.store(false);
        if (onModelsUpdated) onModelsUpdated();
    });
}

std::vector<ModelInfo> SessionRuntime::getAvailableModels() const {
    std::lock_guard<std::mutex> lk(modelsMutex_);
    return availableModels_;
}

void SessionRuntime::joinModelsThread() {
    if (modelsThread_.joinable()) {
        try {
            modelsThread_.join();
        } catch (...) {
        }
    }
}

void SessionRuntime::joinAgentThread() {
    if (agentThread_.joinable()) {
        try {
            agentThread_.join();
        } catch (...) {
        }
    }
}

bool SessionRuntime::stopAgentThread() {
    // No deadline. Either the worker has finished (then it is joined and the caller
    // may clean up), or it has not -- and then the caller must skip every cleanup
    // step, because the thread may still be writing the session database. Walking
    // away is safe; closing the database under a live writer is not.
    const bool finished = worker_wait::tryJoin(agentThread_, agentLatch_);
    if (!finished) debugLogWarn("[runtime] worker still running at exit; leaving without cleanup");
    return finished;
}

void SessionRuntime::shutdown() {
    std::shared_ptr<SkillRunner> runner;
    {
        std::lock_guard<std::mutex> lk(skillMutex_);
        runner = skillRunner_;
    }
    if (runner) runner->cancel();
    if (agent_) agent_->cancel();
    // ONE question, no deadline: has the worker finished? If it has, everything
    // below is safe. If it has not, NONE of it is safe -- the thread may be writing
    // the session database, and deleting the agent or closing the DB under it is a
    // use-after-free. So: leave the process to the OS, which reclaims the address
    // space atomically, and let SQLite replay its write-ahead log next start.
    if (!stopAgentThread()) return;
    joinModelsThread();    // Detached children: kill them and join their monitor thread. A background build
    // that outlives the process would keep writing to a file nobody reads. The Job
    // Object (KILL_ON_JOB_CLOSE) is the second line of defence for a hard crash.
    taskRegistry_.shutdown();
    // MCP last: killing servers is only safe once no callTool can be in flight,
    // and every server's worker thread is joined here (never detached).
    if (mcpManager_) mcpManager_->shutdownAll();
    tools_.setProcessRunner(nullptr); // detach before main deletes the runner
    delete agent_;
    agent_ = nullptr;
    // Explicitly close the DB and checkpoint the WAL on exit.
    storage_.closeDatabase();
    if (terminalStatusEnabled_) terminal_status::shutdown();
}

void SessionRuntime::emergencyShutdown() {
    std::shared_ptr<SkillRunner> runner;
    {
        std::lock_guard<std::mutex> lk(skillMutex_);
        runner = skillRunner_;
    }
    if (runner) runner->cancel();
    if (agent_) agent_->cancel();
    // Closing the window is a request to leave, not to wait. If the worker is still
    // unwinding there is nothing safe to do except go: no DB close, no agent delete.
    if (!stopAgentThread()) return;
    // It DID finish, so the clean path is available (and free): checkpoint the WAL
    // and leave `error` as the last record instead of a silent clear, so an
    // interrupted run stays visible in the host's list.
    storage_.closeDatabase();
    if (terminalStatusEnabled_) terminal_status::abandon("interrupted");
}
