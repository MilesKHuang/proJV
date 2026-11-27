// proJV -- SessionRuntime: the whole orchestration, with no view attached (08).
//
// Before this class, the turn loop, the slash-command dispatch, the skill phase,
// the MCP tool-table sync and the session bookkeeping all lived in TuiApp, so
// "run one turn" was only reachable through a terminal. Everything that is not
// drawing now lives here:
//
//     submit(text)  -> dispatch one user line (commands, @images, skill trigger)
//     run()         -> do the work, blocking, on the calling thread
//     cancel()      -> Esc / Ctrl+C / shutdown
//     onEvent       -> the only way out (events.h)
//
// A front-end (TuiApp, the `run` subcommand, a future GUI) owns no logic: it
// submits lines, calls run() or startAsync(), and renders the events.
//
// Threading: run() and startAsync() never overlap (one `busy_` latch). Callbacks
// fire on the thread that runs the turn -- a front-end must not touch its widget
// tree from inside them, only queue a redraw.
#pragma once

#include "models.h"
#include "config.h"
#include "storage.h"
#include "agent.h"
#include "events.h"
#include "mcp_manager.h"
#include "skill_runner.h"
#include "task_registry.h"
#include "turn_requests.h"
#include "worker_wait.h"
#include "tools/registry.h"
#include "tools/tool_paths.h"
#include "tools/python_tool_manager.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

class IProcessRunner;

class SessionRuntime {
  public:
    SessionRuntime();
    ~SessionRuntime();

    // Platform layer: one process runner for the whole process (see
    // iprocess_runner.h). Must be set before initialize().
    void setProcessRunner(IProcessRunner* r) { procRunner_ = r; }

    // The workspace this session's tools resolve against. Must be called before
    // initialize(); the default is the launch directory (getWorkspaceDir), so the
    // TUI and `projv run` keep exactly the behaviour they had.
    //
    // One window can host several sessions rooted in different directories, which
    // is why this is a member and not a process-wide read: `chdir` would move every
    // other session's tools (and its snapshot store) with it.
    void setWorkspace(const std::string& dir);
    const std::string& workspace() const { return workspace_; }
    // The roots this session may touch. The studio's file tree marks rows outside
    // them, so a refusal is visible before the click instead of after.
    const ToolPaths& toolPaths() const { return paths_; }

    // Everything TuiApp::initialize() used to do, minus the theme, the terminal
    // status channel and the model list refresh trigger.
    bool initialize();

    // --- the three entry points (design 08) -------------------------------
    // Dispatch one user line. Returns true when a turn is now pending, i.e. the
    // caller should follow with run() (headless) or startAsync() (a UI).
    // Every slash command and the @image markers are handled HERE: a front-end
    // that re-implements them would be the second source of truth.
    bool submit(const std::string& text);

    // Run the pending turn to completion on the calling thread. No-op when
    // nothing is pending or another runner is active.
    void run();

    // Same as run(), on a worker thread; onTurnComplete fires when it returns.
    void startAsync();

    // Start an installed skill as its own phase ("/<skill> <topic>").
    void startSkill(const std::string& name, const std::string& topic,
                    const std::string& rawInput = std::string());

    void cancel();
    bool isBusy() const { return busy_.load(); }
    size_t pendingCount() const;
    // Pop one queued user line and start it. A front-end calls this once the
    // previous turn is over (TuiApp does it from its per-frame sync).
    void drainPendingQueue();

    // --- events out -------------------------------------------------------
    std::function<void(const events::Event&)> onEvent;
    // A one-line system message for the chat (a slash-command reply, an error
    // summary). Fires on the turn thread; the TUI appends it to its bubbles.
    std::function<void(const std::string&)> onNotice;
    std::function<void()> onTurnComplete;
    std::function<void()> onStreamingTick;
    std::function<void()> onModelsUpdated;
    // The visible history must be rebuilt from scratch (new chat / switch).
    std::function<void()> onSessionReset;
    // Terminal phase changes, for a host that paints a status line (OSC 7501).
    // Headless leaves it unset.
    std::function<void(AgentPhase, const std::string&)> onPhase;

    // --- state, read-only or forwarded ------------------------------------
    Agent* agent() { return agent_; }
    const Agent* agent() const { return agent_; }
    AppConfig& config() { return config_; }
    const AppConfig& config() const { return config_; }
    mcp::Manager* mcpManager() { return mcpManager_.get(); }
    const std::vector<std::string>& promptFiles() const { return promptFiles_; }
    int activePromptIndex() const { return activePromptIndex_; }
    // Whether /undo can work in this session's workspace (see snapshot.h).
    bool snapshotsAvailable() const { return snapshot::isAvailable(snapshotStore_); }
    const std::string& skillsDir() const { return skillsDir_; }
    std::vector<bg_task::Snapshot> taskSnapshots() const;
    std::vector<std::string> installedSkills() const;
    std::string skillStatus() const;
    std::vector<SkillRunner::AgentSnapshot> skillAgentStatuses() const;
    std::vector<std::string> listSessions() const;
    TodoData copyTodoData() const { return agent_ ? agent_->copyTodoData() : TodoData{}; }
    AgentStatus status() const { return agent_ ? agent_->getStatus() : AgentStatus{}; }
    AgentPhase phase() const { return agent_ ? agent_->getPhase() : AgentPhase::Idle; }
    int promptTokens() const { return totalPromptTokens_.load(std::memory_order_relaxed); }
    int completionTokens() const { return totalCompletionTokens_.load(std::memory_order_relaxed); }

    // --- actions a front-end drives ---------------------------------------
    void approveTool(int action);
    void saveApiKeyAndConnect(const std::string& apiKey);
    void saveFullConfig(const AppConfig& next);
    void switchPrompt(int index);
    // newChat / switchToDialog / setModel change the live session or the model, so
    // they do nothing while a turn runs: they call cancel() or close the database,
    // and a third caller must not be able to race the worker. The front-end refuses
    // first (it has the sentence to show); the guard here is the backstop.
    void newChat();
    void switchToDialog(const std::string& dbPath);
    // Give the session a fixed database file, creating it when it is not there.
    // The headless `run --session` front-end needs one session across two
    // processes: the snapshot index and /undo are keyed on the database name, so
    // without this a second run cannot undo the first one's turn.
    bool useSessionFile(const std::string& dbPath);
    void setModel(const std::string& model);
    void setMcpEnabled(const std::string& name, bool enabled);
    void refreshModels();
    std::vector<ModelInfo> getAvailableModels() const;
    bool modelsLoading() const { return modelsLoading_.load(); }
    void refreshEnvironmentBlock();

    void shutdown();
    void emergencyShutdown();

    // Headless: `run` must never paint the host terminal (its OSC sequences would
    // land in the middle of the JSONL stream).
    void setTerminalStatusEnabled(bool on) { terminalStatusEnabled_ = on; }

  private:
    void setupTools();
    std::string activePromptContent() const;
    void injectFixedBlocks();
    void refreshCapabilitiesBlock();
    void applyConfigToAgent();
    void reinitConversation(bool keepStorage);
    void runTurnLoop();
    void doWork();
    bool runSkillBlocking(const std::string& name, const std::string& topic,
                          const std::string& rawInput);
    void joinAgentThread();
    void joinModelsThread();
    // Exit decision, not a deadline: join the worker when it has already finished,
    // otherwise detach it and return false -- the caller then cleans up NOTHING.
    bool stopAgentThread();
    std::string buildSharedContext();

    // Event + notice plumbing. notice() is what the old code did with a
    // bubble_model::Bubble push; the front-end turns it into a bubble.
    void emit(events::Event e);
    void notice(const std::string& text);
    int nextSeq() { return ++seq_; }

    struct PendingTurn {
        std::string text;
        std::vector<ImageAttachment> images;
    };
    std::optional<PendingTurn> pendingTurn_;
    // A user-triggered skill ("/<skill> <topic>"): submit() only records it and
    // doWork() runs it, so the headless path and the TUI share one code path.
    struct PendingSkillRun {
        std::string name;
        std::string topic;
        std::string rawInput;
    };
    std::optional<PendingSkillRun> pendingSkillRun_;
    AppConfig config_;
    DeepSeekClient client_;
    ToolRegistry tools_;
    ToolPaths paths_;
    std::string workspace_; // setWorkspace() or the launch directory
    snapshot::Store snapshotStore_ = snapshot::kNoStore; // this workspace's /undo store
    std::optional<PythonToolManager> pytoolMgr_;
    std::unique_ptr<mcp::Manager> mcpManager_;
    std::atomic<bool> toolsReloadRequested_{false};
    Storage storage_;
    Agent* agent_ = nullptr;
    IProcessRunner* procRunner_ = nullptr;
    TaskRegistry taskRegistry_;

    std::string skillsDir_;
    std::string sessionsDir_;
    std::vector<std::string> promptFiles_;
    int activePromptIndex_ = 0;

    std::vector<std::string> pendingQueue_;
    std::thread agentThread_;
    // Signalled by the worker as its last statement, so an exit can tell "finished"
    // from "stuck" instead of blocking in join() forever.
    worker_wait::Latch agentLatch_;
    std::atomic<bool> busy_{false};
    std::atomic<bool> terminalStatusEnabled_{true};

    // Skill requested by the run_skill tool during the current turn.
    struct PendingSkill {
        std::string name;
        std::string topic;
    };
    std::optional<PendingSkill> pendingSkill_;
    struct PendingImages {
        std::vector<ImageAttachment> images;
    };
    std::optional<PendingImages> pendingImages_;
    turn_requests::State turnRequest_;
    int skillCallsThisTurn_ = 0;
    std::string lastSkillThisTurn_;

    std::shared_ptr<SkillRunner> skillRunner_;
    std::string skillStatus_;
    mutable std::mutex skillMutex_;

    std::vector<ModelInfo> availableModels_;
    mutable std::mutex modelsMutex_;
    std::atomic<bool> modelsLoading_{false};
    std::thread modelsThread_;

    std::atomic<int> totalPromptTokens_{0};
    std::atomic<int> totalCompletionTokens_{0};
    int seq_ = 0;
    int turnSeq_ = 0;
};
