// proJV studio -- the front-end's state and actions (see studio_app.h).
#include "studio_app.h"

#include "core/help_text.h"
#include "core/snapshot.h"
#include "core/speaker_slots.h"
#include "design/path_text.h"
#include "platform/iprocess_runner.h"
#include "platform/isystem_stats.h"
#include "platform/isystem_util.h"
#include "tui/approval_logic.h"
#include "tui/paste_flatten.h"
#include "tui/theme_manager.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <cstdio>
#include <filesystem>

namespace studio {
namespace {

constexpr size_t kMaxBubbles = 200; // bound the rendering cost, same cap as TuiApp
constexpr int kMaxPanes = 5;        // the reference window's MAX_PANES

std::string stripMd(const std::string& name) {
    if (name.size() > 3 && name.substr(name.size() - 3) == ".md")
        return name.substr(0, name.size() - 3);
    return name;
}

int intOr(const std::string& s, int fallback) {
    try {
        return std::stoi(s);
    } catch (...) {
        return fallback;
    }
}

double doubleOr(const std::string& s, double fallback) {
    try {
        return std::stod(s);
    } catch (...) {
        return fallback;
    }
}


long long monotonicMs() {
    using namespace std::chrono;
    return static_cast<long long>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// "proJV @ projV": the program and the directory it was started in. The reference
// panel names its sessions the same way, and the name is the only thing that tells
// two panes apart in a list.
std::string paneTitleFor(const std::string& workspace) {
    std::error_code ec;
    std::string leaf = std::filesystem::path(workspace).filename().string();
    if (leaf.empty()) leaf = workspace;
    if (leaf.empty()) leaf = "projV";
    return "projV @ " + leaf;
}

} // namespace

App::App() = default;

App::~App() {
    stopSampling();
    pendingInit_.clear(); // raw pointers: drop them before their owners
    panes_.clear();
}

// Answers for "there is no pane". Closing the last pane leaves the window with an
// empty shell, and the prompt, the header and F4 keep running: without this, every
// one of those paths dereferences panes_[0] on an empty vector -- that is the crash
// F4 caused once the last card was gone. Writes land here and are dropped on
// purpose: there is no session to write to.
static Pane& noPane() {
    static Pane pane;
    return pane;
}

Pane& App::active() {
    if (panes_.empty() || active_ < 0 || active_ >= static_cast<int>(panes_.size()))
        return noPane();
    return *panes_[static_cast<size_t>(active_)];
}

const Pane& App::active() const {
    if (panes_.empty() || active_ < 0 || active_ >= static_cast<int>(panes_.size()))
        return noPane();
    return *panes_[static_cast<size_t>(active_)];
}

void App::bindActive() {
    if (panes_.empty()) {
        runtime_ = nullptr;
        active_ = 0;
        return;
    }
    if (active_ < 0) active_ = 0;
    if (active_ >= static_cast<int>(panes_.size())) active_ = static_cast<int>(panes_.size()) - 1;
    // A pane that is still starting has a runtime whose agent and database do not
    // exist yet: the turn-driving code must see "no runtime rather than a half-
 // built one. That read was the access violation reported on + New.
 Pane& pane = *panes_[static_cast<size_t>(active_)];
 runtime_ = pane.state.load() == Pane::State::Ready ? pane.rt.get() : nullptr;
}

// The callbacks below run on that pane's TURN thread. None of them touches a
// widget: they queue a string or set a flag, and tick() reads it on this thread.
void App::wireRuntime(Pane& pane) {
    Pane* p = &pane;
    App* self = this;
    pane.rt->onNotice = [p](const std::string& text) {
        std::lock_guard<std::mutex> lk(p->noticeMutex);
        p->notices.push_back(text);
    };
    pane.rt->onSessionReset = [p, self] {
        p->chat.clear();
        p->chatKeys.clear();
        p->lastMessageId = 0;
        p->followBottom = true;
        if (self->runtime_ && self->runtime_ == p->rt.get()) self->promptReload_ = false;
    };
    // OSC 7501 is a terminal feature. A window has no host terminal to report to,
    // so the studio leaves the phase channel unset on purpose.
}

bool App::initialize(IProcessRunner* runner) {
    runner_ = runner;
    std::string why;
    if (!newPane(std::string(), &why, true)) return false;
    if (!runtime_) return false;
    ThemeManager::instance().init(getThemeDir(), runtime_->config().themeName);
    ThemeManager::instance().onThemeChanged = [this](const std::string& name) {
        if (!runtime_) return;
        runtime_->config().themeName = name;
        saveConfig(runtime_->config());
        themeStatus_ = "theme: " + name;
    };
    slashAll_ = slash_menu::all(runtime_->installedSkills(), runtime_->skillsDir());
    refreshModels();
    if (!hasApiKey()) openWelcomeModal();
    firstPaneReady_ = true;
    finishStartup_ = true;
    return true;
}

bool App::newPane(const std::string& workspace, std::string* why, bool syncNow) {
    // Five, the reference window's own limit: every pane carries a runtime, a worker
    // thread and its own MCP servers, so this is a budget, not an opinion.
    if (static_cast<int>(panes_.size()) >= kMaxPanes) {
        if (why) *why = "pane limit reached (5): close one first";
        setReceipt("pane limit reached (5): close one first");
        return false;
    }
    auto pane = std::make_unique<Pane>();
    pane->rt = std::make_unique<SessionRuntime>();
    pane->rt->setProcessRunner(runner_);
    if (!workspace.empty()) pane->rt->setWorkspace(workspace);
    pane->workspace = workspace.empty() ? getWorkspaceDir() : workspace;
    pane->title = paneTitleFor(pane->workspace);
    pane->state.store(Pane::State::Starting);
    Pane* raw = pane.get();
    wireRuntime(*pane);
    panes_.push_back(std::move(pane));
    refreshPaneTitles();
    activatePane(static_cast<int>(panes_.size()) - 1);

    // initialize() builds the runtime: config, tools, agent, prompts, the fixed
    // context blocks and the MCP spawn. Two ways, and both matter:
    //   - the FIRST pane (start-up): here, on this thread, before the window exists;
    //   - every later pane: queued for tick(), which runs it on the next frame -- by
    //     then the new card is already on screen showing "starting...", so "+"
    //     answers immediately instead of looking dead.
    // A worker thread was tried for this and CRASHED inside initialize()
    // (SessionRuntime is not thread-safe to build off the UI thread), so the work
    // stays on this thread and only the *feedback* is asynchronous.
    // The measured cost is in docs/en|zh/dev-guide/studio-gui.md; it is NOT the
    // 3-5 s this comment used to claim.
    if (syncNow) {
        const bool ok = raw->rt && raw->rt->initialize();
        if (ok) raw->workspace = raw->rt->workspace();
        raw->state.store(ok ? Pane::State::Ready : Pane::State::Failed);
        if (!ok) raw->error = "initialize failed (see projv_files/logs)";
        bindActive();
        if (ok) adoptFinishedPanes();
    } else {
        // A QUEUE, not one slot: two clicks in the same frame used to overwrite the
        // first index, and that pane stayed Starting for ever -- closePane() refuses
        // to close a Starting pane, so it was stuck with no way out.
        pendingInit_.push_back(raw);
    }
    // `pane` was MOVED into panes_ above: read through the raw pointer, or this line
    // dereferences a null unique_ptr (the access violation that killed start-up).
    setReceipt("starting " + raw->title + " in " + raw->workspace + " ...");
    return true;
}

void App::refreshPaneTitles() {
    // Two panes in folders whose last component is the same ("...\\\\Release") would read
    // identically in the list. The first keeps the plain name, the rest get " (2)",
    // " (3)", and so on, in list order -- recomputed after every add or close, so the
    // numbers never have a gap.
    std::map<std::string, int> seen;
    for (auto& owned : panes_) {
        Pane& pane = *owned;
        const std::string base = paneTitleFor(pane.workspace);
        int& count = seen[base];
        ++count;
        pane.title = count == 1 ? base : base + " (" + std::to_string(count) + ")";
    }
}

bool App::paneReady(int index) const {
    if (index < 0 || index >= static_cast<int>(panes_.size())) return false;
    return panes_[static_cast<size_t>(index)]->state.load() == Pane::State::Ready;
}

void App::adoptFinishedPanes() {
    for (auto& owned : panes_) {
        Pane& pane = *owned;
        if (pane.state.load() != Pane::State::Ready || pane.adopted) continue;
        pane.adopted = true;
        if (pane.workspace.empty()) pane.workspace = pane.rt->workspace();
        refreshPaneTitles();
        if (!firstPaneReady_) {
            firstPaneReady_ = true;
            ThemeManager::instance().init(getThemeDir(), pane.rt->config().themeName);
            ThemeManager::instance().onThemeChanged = [this](const std::string& name) {
                if (!runtime_) return;
                runtime_->config().themeName = name;
                saveConfig(runtime_->config());
                themeStatus_ = "theme: " + name;
            };
            slashAll_ = slash_menu::all(pane.rt->installedSkills(), pane.rt->skillsDir());
            refreshModels();
            if (!hasApiKey()) openWelcomeModal();
        }
        if (&pane == &active()) {
            std::string err;
            tree_.setRoot(pane.workspace, &err);
            promptReload_ = true;
        }
        setReceipt("ready: " + pane.title);
    }
}

void App::activatePane(int index) {
    if (index < 0 || index >= static_cast<int>(panes_.size())) return;
    active_ = index;
    bindActive();
    // A pane switch rebinds the prompt, the chat and the file tree: the tree follows
    // the directory of the session you are looking at (the reference panel does the
    // same, and it is why the two panels feel like one switchboard).
    std::string err;
    tree_.setRoot(active().workspace, &err);
    treeSelected_ = 0;
    promptReload_ = true;
}

void App::shutdown() {
    stopSampling();
    for (auto& pane : panes_) {
        if (pane && pane->rt && pane->state == Pane::State::Ready) pane->rt->shutdown();
    }
}

void App::emergencyShutdown() {
    for (auto& pane : panes_) {
        if (pane && pane->rt) pane->rt->emergencyShutdown();
    }
}

bool App::hasApiKey() const {
    return runtime_ && runtime_->config().loaded && !runtime_->config().apiKey.empty();
}

bool App::paneBusy(int index) const {
    if (index < 0 || index >= static_cast<int>(panes_.size())) return false;
    const Pane& pane = *panes_[static_cast<size_t>(index)];
    return pane.state.load() == Pane::State::Ready && pane.rt->isBusy();
}

const std::string& App::paneTitle(int index) const {
    static const std::string empty;
    if (index < 0 || index >= static_cast<int>(panes_.size())) return empty;
    return panes_[static_cast<size_t>(index)]->title;
}

const std::string& App::paneWorkspace(int index) const {
    static const std::string empty;
    if (index < 0 || index >= static_cast<int>(panes_.size())) return empty;
    return panes_[static_cast<size_t>(index)]->workspace;
}

const view_model::Screen& App::screenOf(int index) const {
    static const view_model::Screen empty;
    if (index < 0 || index >= static_cast<int>(panes_.size())) return empty;
    return panes_[static_cast<size_t>(index)]->screen;
}

const std::string& App::paneStateName(int index) const {
    static const std::string ready = "ready";
    static const std::string starting = "starting...";
    static const std::string failed = "failed";
    if (index < 0 || index >= static_cast<int>(panes_.size())) return ready;
    switch (panes_[static_cast<size_t>(index)]->state) {
    case Pane::State::Starting:
        return starting;
    case Pane::State::Failed:
        return failed;
    case Pane::State::Ready:
    default:
        return ready;
    }
}

bool App::closePane(int index) {
    if (index < 0 || index >= static_cast<int>(panes_.size())) return false;
    Pane& pane = *panes_[static_cast<size_t>(index)];
    // One ending, and it may not orphan a worker or a startup thread: a busy pane
    // must be stopped first (Esc), a starting one must finish.
    if (pane.state.load() == Pane::State::Starting) {
        setReceipt("pane is still starting: wait a moment");
        return false;
    }
    if (pane.state.load() == Pane::State::Ready && pane.rt->isBusy()) {
        setReceipt("pane is running a turn: Esc stops it first");
        return false;
    }
    if (pane.state.load() == Pane::State::Ready) pane.rt->shutdown();
    panes_.erase(panes_.begin() + index);
    refreshPaneTitles();
    if (panes_.empty()) {
        bindActive();
        return true;
    }
    activatePane(std::min(active_, static_cast<int>(panes_.size()) - 1));
    setReceipt("pane closed");
    return true;
}

float App::sourceCost() const {
    if (!runtime_) return 0.0f;
    const AppConfig& cfg = runtime_->config();
    return static_cast<float>(runtime_->promptTokens() *
                                  lookupModelPrice(cfg.modelPrices, cfg.model, true) * 1e-6 +
                              runtime_->completionTokens() *
                                  lookupModelPrice(cfg.modelPrices, cfg.model, false) * 1e-6);
}

// ---- chat projection -----------------------------------------------------

void App::harvestSpeakers(Pane& pane) {
    if (!pane.rt || pane.state.load() != Pane::State::Ready) return;
    for (const auto& a : pane.rt->skillAgentStatuses()) {
        if (!a.displayName.empty()) pane.knownSpeakers.insert(a.displayName);
    }
}
void App::fillSource() {
    view_model::Source s;
    s.version = PROJV_VERSION;
    s.modal = modal_;
    s.dock = dock_;
    s.phase = demoBusy_ ? AgentPhase::ExecutingTools
                        : (runtime_ ? runtime_->phase() : AgentPhase::Idle);
    s.busy = busy() || demoBusy_;
    s.spinnerFrame = spinnerFrame_;
    s.model = runtime_ ? runtime_->config().model : std::string();
    // The pane's own folder, not the launch/configured one: a pane started in
    // another folder showed this window's workspace in its service bar.
    s.workspace = active().workspace.empty() ? getWorkspaceDir() : active().workspace;
    s.promptTokens = runtime_ ? runtime_->promptTokens() : 0;
    s.completionTokens = runtime_ ? runtime_->completionTokens() : 0;
    s.cost = sourceCost();

    if (runtime_) {
        const int idx = runtime_->activePromptIndex();
        const auto& files = runtime_->promptFiles();
        s.roleIndex = idx;
        s.promptFiles = static_cast<int>(files.size());
        if (idx >= 0 && idx < static_cast<int>(files.size()))
            s.roleName = stripMd(files[static_cast<size_t>(idx)]);
        s.skillStatus = runtime_->skillStatus();
        s.queued = static_cast<int>(runtime_->pendingCount());
    }
    if (runtime_ && runtime_->agent()) {
        Agent* a = runtime_->agent();
        s.status = a->getStatus();
        // Streaming text grows BETWEEN messages, so an appended bubble is not the
        // only "content arrived" event: each growth is a pulse too, or the chase
        // would stop in the middle of an answer. It only ever asks; drawChat decides
        // from the reader's distance and whether they are moving.
        const size_t streamed = s.status.streamingText.size() + s.status.reasoningText.size();
        if (streamed != streamedChars_) {
            streamedChars_ = streamed;
            if (streamed > 0) active().followBottom = true;
        }
        s.msgCount = a->getSession().messageCount();
        int toolCount = 0;
        for (const auto& m : a->getSession().getContextMessages()) {
            if (m.role == "tool") ++toolCount;
        }
        s.toolCount = toolCount;
        s.pressure = static_cast<int>(a->getContextPressure());
        s.estimatedTokens = a->getEstimatedContextTokens();
        s.windowTokens = a->getContextWindowSize();
    }
    s.bubbles = active().chat;
    s.bubbleKeys = active().chatKeys;
    s.toolsCollapsed = toolsCollapsed_;
    s.skillsCollapsed = skillsCollapsed_;
    // std::set, so the order is sorted: build() must stay deterministic.
    s.expandedKeys.assign(expandedKeys_.begin(), expandedKeys_.end());
    s.reasoningExpanded = reasoningExpanded_;

    // Agent strip: the same cells the TUI lays out, from the same snapshot.
    if (runtime_) {
        for (const auto& a : runtime_->skillAgentStatuses()) {
            view_model::AgentRow row;
            row.name = a.displayName;
            row.tag = "[" + view_model::truncateToCols(a.displayName, 10) + "]";
            row.idle = a.activity == SkillRunner::AgentActivity::Idle;
            row.activity = row.idle
                               ? "idle"
                               : (a.activity == SkillRunner::AgentActivity::Error ? "error"
                                                                                 : "working");
            row.slot = static_cast<int>(speaker_slots::paletteSlot(a.displayName));
            std::string detail = view_model::lastNarrationLine(a.status);
            if (detail.empty()) detail = row.idle ? std::string() : (row.activity + "...");
            const int room = row.unitCols - static_cast<int>(row.tag.size()) - 2;
            if (room > 0) row.detail = view_model::truncateToCols(detail, room);
            s.agents.push_back(std::move(row));
        }
        for (const auto& t : runtime_->taskSnapshots()) {
            if (t.state != bg_task::State::Running) continue;
            view_model::TaskRow row;
            row.tag = "bg#" + std::to_string(t.id);
            row.activity = "working";
            row.detail = t.lastLine.empty() ? t.command : t.lastLine;
            s.tasks.push_back(std::move(row));
        }
        s.todoText = runtime_->copyTodoData().pendingTodo;
    }

    s.slashRows.clear();
    for (const auto& c : slashRows_) s.slashRows.push_back({c.name, c.args, c.desc});
    s.slashOpen = slashOpen_;
    s.slashSelected = slashSelected_;

    s.input.text = active().input;
    s.input.cursor = static_cast<int>(active().input.size());
    s.input.placeholder = "type a message - Enter sends, Shift+Enter starts a new line";

    fillModalSource(s);
    active().source = std::move(s);
}

// The open modal's rows. Only one is ever filled, so this is a chain, not a block
// of independent checks -- the TUI has the same "one visible modal" rule.
void App::fillModalSource(view_model::Source& s) {
    if (modal_ == "config") {
        s.config.visible = true;
        s.config.title = "Configuration (F2)";
        s.config.hint =
            "Esc or F2 closes | max_tokens 1024-65536; effort none/low/high/max; context 0 = auto";
        const auto push = [&](const char* label, const std::string& value, const char* kind) {
            view_model::ConfigField f;
            f.label = label;
            f.value = value;
            f.kind = kind;
            s.config.fields.push_back(std::move(f));
        };
        push("API Key", form_.apiKey, "password");
        push("Base URL", form_.baseUrl, "text");
        push("Model", form_.model, "text");
        push("Max Tokens", form_.maxTokens, "number");
        push("Temperature", form_.temperature, "number");
        {
            view_model::ConfigField f;
            f.label = "Effort";
            f.kind = "radio";
            f.options = {"none -- no thinking", "low", "high (official default)", "max"};
            f.index = form_.effortIndex;
            s.config.fields.push_back(std::move(f));
        }
        push("Context Window", form_.contextWindow, "number");
        push("C++ Compiler", form_.cppCompiler, "text");
        push("Python", form_.python, "text");
        push("clang-format", form_.clangFormat, "text");
        push("Snapshot GB", form_.snapGb, "number");
        push("Snapshot days", form_.snapDays, "number");
        push("Theme", form_.theme, "text");
        {
            view_model::ConfigField f;
            f.label = "osc_status -- report state to the terminal (OSC 7501)";
            f.kind = "check";
            f.checked = form_.osc;
            s.config.fields.push_back(std::move(f));
        }
        {
            view_model::ConfigField f;
            f.label = "Command approval";
            f.kind = "radio";
            f.options = {"Strict -- everything asks, nothing remembered",
                         "Balanced -- destructive asks, build output runs",
                         "Loose -- only format/partition is refused"};
            f.index = form_.modeIndex;
            s.config.fields.push_back(std::move(f));
        }
        push("remembered", std::to_string(form_.cfg.approvalGrants.size()) + " command(s)", "note");
        s.config.buttons = {"Save", "Cancel"};
        return;
    }
    if (modal_ == "mcp") {
        s.mcp.visible = true;
        if (mcp::Manager* m = runtime_ ? runtime_->mcpManager() : nullptr) {
            for (const auto& name : m->serverNames()) {
                view_model::McpRow r;
                r.enabled = m->enabledOf(name);
                r.trusted = m->trustedOf(name);
                r.name = name;
                r.protocol = m->protocolOf(name);
                r.state = mcp::connStateName(m->connState(name));
                r.tools = static_cast<int>(m->toolCount(name));
                s.mcp.rows.push_back(std::move(r));
            }
            const auto names = m->serverNames();
            if (listSelected_ >= 0 && listSelected_ < static_cast<int>(names.size())) {
                s.mcp.purpose = m->descriptionOf(names[static_cast<size_t>(listSelected_)]);
                s.mcp.error = m->lastError(names[static_cast<size_t>(listSelected_)]);
            }
        }
        s.mcp.selected = listSelected_;
        s.mcp.hint = busy() ? "busy -- finish the current turn first"
                            : "Enter toggle | R restart | Esc close";
        s.mcp.buttons = {"Restart", "Done"};
        return;
    }
    if (modal_ == "open") {
        s.open.visible = true;
        s.open.title = "Open Session";
        s.open.hint = "Esc close | Up/Down select | Enter open";
        for (const auto& p : sessionPaths()) s.open.rows.push_back(p);
        if (s.open.rows.empty()) s.open.rows.push_back("(no sessions)");
        s.open.selected = listSelected_;
        return;
    }
    if (modal_ == "model") {
        s.modelList.visible = true;
        s.modelList.title = "Model";
        s.modelList.hint = "Esc close | Up/Down select | Enter apply";
        s.modelList.rows = modelRows_;
        s.modelList.selected = listSelected_;
        return;
    }
    if (modal_ == "theme") {
        s.themeList.visible = true;
        s.themeList.title = "Theme";
        s.themeList.hint = "Esc close | Up/Down select | Enter apply";
        s.themeList.rows = themeRows_;
        s.themeList.selected = listSelected_;
        return;
    }
    if (modal_ == "theme_editor") {
        s.themeEditor.visible = true;
        s.themeEditor.title = "Theme Editor";
        for (const auto& f : themeColorFields()) s.themeEditor.fields.push_back(f.name);
        s.themeEditor.selected = themeField_;
        if (themeField_ >= 0 && themeField_ < static_cast<int>(themeColorFields().size()))
            s.themeEditor.field = themeColorFields()[static_cast<size_t>(themeField_)].name;
        s.themeEditor.hex = themeHex_;
        s.themeEditor.hint = "Esc close | Up/Down field | Enter edit hex";
        s.themeEditor.buttons = {"Apply", "Save", "Close"};
        return;
    }
    if (modal_ == "help") {
        s.help.visible = true;
        s.help.title = "Help (F1)";
        for (const auto& h : help_text::quickHints()) s.help.rows.push_back({h.key, h.desc});
        s.help.hint = "Enter, Esc or F1 closes";
        s.help.buttons = {"Close"};
        return;
    }
    if (modal_ == "quit") {
        s.quit.visible = true;
        s.quit.title = "Quit proJV?";
        s.quit.body = "The session is saved and the database is closed.";
        s.quit.buttons = {"Cancel", "Quit"};
        return;
    }
    if (modal_ == "welcome") {
        s.welcome.visible = true;
        s.welcome.title = "Welcome to proJV";
        s.welcome.body = "You need a DeepSeek API key to get started.";
        s.welcome.keyLabel = "API Key: ";
        s.welcome.button = "Save & Connect";
        s.welcome.hint = "Commands: /help, /workspace, /clear, /compress, /skill";
        return;
    }
    if (modal_ == "approval") {
        s.approval.visible = true;
        s.approval.title = "COMMAND APPROVAL";
        s.approval.intro = "This command deletes or overwrites files:";
        std::optional<ToolCall> call;
        if (runtime_ && runtime_->agent()) call = runtime_->agent()->pendingApproval();
        std::string cmd;
        if (call) cmd = approval_logic::extractCommand(call->arguments);
        s.approval.command = cmd.empty() ? "(arguments unreadable)" : cmd;
        const auto files = approval_logic::extractDeleteFiles(cmd);
        const size_t shown = std::min<size_t>(files.size(), 10);
        for (size_t i = 0; i < shown; ++i) s.approval.targets.push_back(files[i]);
        s.approval.moreTargets = static_cast<int>(files.size() - shown);
        const bool canAlways = approvedAlways();
        s.approval.showAlways = canAlways;
        s.approval.explain =
            canAlways
                ? "ALWAYS remembers this exact command in config.toml; NO drops the whole batch."
                : "strict mode: nothing is remembered; NO drops the whole batch.";
        s.approval.undo =
            (runtime_ && runtime_->snapshotsAvailable())
                ? "[Undo available] the files this turn changes can be restored with /undo"
                : "[Undo unavailable] a deletion here cannot be reverted";
        s.approval.buttons = {"YES -- Execute Once", "ALWAYS -- Allow This Command",
                              "NO -- Cancel"};
    }
}

// ---- input line ----------------------------------------------------------

void App::setInputText(const std::string& s) {
    active().input = s;
    onInputEdited();
}

void App::commitInput(const char* text) {
    if (!text) return;
    // The field is multi-line, so a line break is something the user asked for with
    // Shift+Enter and it stays (the single-line field had to flatten it away). A
    // tab becomes a space -- a tab in a buffer that is sent to a model renders as
    // nothing in the transcript -- a pasted CRLF pair contributes one newline, and
    // an absurd paste is capped, exactly like the TUI's paste_flatten.
    std::string next;
    for (const char* p = text; *p; ++p) {
        if (*p == '\r') continue;
        next.push_back(*p == '\t' ? ' ' : *p);
    }
    if (next.size() > paste_flatten::kMaxFlattenChars) {
        next.resize(paste_flatten::kMaxFlattenChars);
    }
    if (next == active().input) return;
    active().input = std::move(next);
    onInputEdited();
}

void App::onInputEdited() {
    // Only a leading "/" with no separator yet can be a command name. Typing the
    // first character of the arguments ("/wish a...") closes the menu, which is
    // exactly what the user means -- and a newline after the name counts as that
    // separator, or the menu would hang over the composer for good.
    const size_t sp = active().input.find_first_of(" \t\n");
    const bool bare = !active().input.empty() && active().input[0] == '/' && sp == std::string::npos;
    if (!bare) {
        slashOpen_ = false;
        return;
    }
    slashRows_ = slash_menu::filter(slashAll_, active().input);
    slashSelected_ = 0;
    slashOpen_ = !slashRows_.empty();
}

void App::slashMove(int delta) {
    const int n = static_cast<int>(slashRows_.size());
    if (n <= 0) return;
    slashSelected_ = (slashSelected_ + delta + n) % n;
}

void App::slashSelect(int index) {
    const int n = static_cast<int>(slashRows_.size());
    if (index < 0 || index >= n) return;
    slashSelected_ = index;
}

void App::slashFill() {
    if (slashSelected_ < 0 || slashSelected_ >= static_cast<int>(slashRows_.size())) return;
    active().input = slash_menu::completion(slashRows_[static_cast<size_t>(slashSelected_)]);
    slashOpen_ = false;
}

void App::slashClose() {
    slashOpen_ = false;
}

// ---- modals --------------------------------------------------------------

void App::openModal(const std::string& name) {
    ++debugModalSets_;
    if (name == "config") {
        openConfigModal();
        return;
    }
    if (name == "welcome") {
        openWelcomeModal();
        return;
    }
    if (name == "approval") {
        openApprovalModal();
        return;
    }
    if (name == "mcp") {
        openMcpModal();
        return;
    }
    if (name == "open") {
        openOpenModal();
        return;
    }
    if (name == "model") {
        openModelModal();
        return;
    }
    if (name == "theme") {
        openThemeModal();
        return;
    }
    if (name == "theme_editor") {
        openThemeEditorModal();
        return;
    }
    if (name == "help") {
        openHelpModal();
        return;
    }
    modal_ = name;
}

void App::openConfigModal() {
    if (!runtime_) return;
    form_.cfg = runtime_->config();
    form_.apiKey = form_.cfg.apiKey;
    form_.baseUrl = form_.cfg.baseUrl;
    form_.model = form_.cfg.model;
    form_.maxTokens = std::to_string(form_.cfg.maxTokens);
    form_.temperature =
        form_.cfg.temperature < 0.0001 ? "0" : std::to_string(form_.cfg.temperature);
    form_.contextWindow = std::to_string(form_.cfg.contextWindow);
    form_.cppCompiler = form_.cfg.cppCompilerPath;
    form_.python = form_.cfg.pythonPath;
    form_.clangFormat = form_.cfg.clangFormatPath;
    form_.snapGb = std::to_string(form_.cfg.snapshotsMaxWorkspaceGb);
    form_.snapDays = std::to_string(form_.cfg.snapshotsKeepDays);
    form_.theme = form_.cfg.themeName;
    form_.modeIndex = static_cast<int>(form_.cfg.approvalMode);
    form_.osc = form_.cfg.oscStatus;
    std::string curEffort = normalizeReasoningEffort(form_.cfg.reasoningEffort);
    if (curEffort.empty()) curEffort = kReasoningEffortOfficialDefault();
    const auto& values = kEffortValues();
    int idx = 2;
    for (size_t i = 0; i < values.size(); ++i) {
        if (values[i] == curEffort) idx = static_cast<int>(i);
    }
    form_.effortIndex = idx;
    modal_ = "config";
}

void App::saveConfigFromForm() {
    if (!runtime_) return;
    AppConfig next = form_.cfg;
    // Clamp exactly where config.cpp clamps on load: the file and the UI must
    // never disagree about what a legal value is.
    next.apiKey = form_.apiKey;
    next.baseUrl = form_.baseUrl;
    next.model = form_.model;
    next.maxTokens =
        std::clamp(intOr(form_.maxTokens, next.maxTokens), kMinOutputTokens, kMaxOutputTokens);
    next.temperature = std::clamp(doubleOr(form_.temperature, next.temperature), 0.0, 2.0);
    next.reasoningEffort = kEffortValues()[static_cast<size_t>(std::clamp(form_.effortIndex, 0, 3))];
    const int cw = intOr(form_.contextWindow, static_cast<int>(next.contextWindow));
    next.contextWindow = cw < 0 ? 0 : static_cast<size_t>(cw);
    next.cppCompilerPath = form_.cppCompiler;
    next.pythonPath = form_.python;
    next.clangFormatPath = form_.clangFormat;
    next.snapshotsMaxWorkspaceGb = std::max(0, intOr(form_.snapGb, next.snapshotsMaxWorkspaceGb));
    next.snapshotsKeepDays = std::max(1, intOr(form_.snapDays, next.snapshotsKeepDays));
    next.themeName = form_.theme;
    next.approvalMode = static_cast<ApprovalMode>(std::clamp(form_.modeIndex, 0, 2));
    next.oscStatus = form_.osc;
    runtime_->saveFullConfig(next);
    closeModal();
}

void App::clearGrants() {
    form_.cfg.approvalGrants.clear();
    if (runtime_ && runtime_->agent()) runtime_->agent()->clearGrants();
}

void App::openWelcomeModal() {
    form_.apiKey.clear();
    modal_ = "welcome";
}

void App::openApprovalModal() {
    modal_ = "approval";
}

void App::openMcpModal() {
    listSelected_ = 0;
    modal_ = "mcp";
}

void App::openOpenModal() {
    listSelected_ = 0;
    listPaths_ = sessionPaths();
    modal_ = "open";
}

void App::openModelModal() {
    refreshModels(); // F10 asks the API again, exactly like the TUI does
    refreshModelRows();
    listSelected_ = 0;
    for (size_t i = 0; i < modelRows_.size(); ++i) {
        if (runtime_ && modelRows_[i] == runtime_->config().model) listSelected_ = static_cast<int>(i);
    }
    modal_ = "model";
}

void App::openThemeModal() {
    refreshThemeRows();
    listSelected_ = 0;
    for (size_t i = 0; i < themeRows_.size(); ++i) {
        if (themeRows_[i] == ThemeManager::instance().current().name)
            listSelected_ = static_cast<int>(i);
    }
    modal_ = "theme";
}

void App::openThemeEditorModal() {
    themeField_ = 0;
    themeHex_ = ThemeManager::instance().current().*(themeColorFields()[0].member);
    modal_ = "theme_editor";
}

void App::openHelpModal() {
    modal_ = "help";
}

// ---- lists ---------------------------------------------------------------

void App::refreshModelRows() {
    modelRows_.clear();
    if (runtime_) {
        auto models = runtime_->getAvailableModels();
        for (const auto& m : models) modelRows_.push_back(m.id);
        if (modelRows_.empty()) {
            if (runtime_->modelsLoading())
                modelRows_.push_back("(loading models...)");
            else {
                modelRows_.push_back("deepseek-v4-flash");
                modelRows_.push_back("deepseek-v4-pro");
            }
        }
    }
}

void App::refreshThemeRows() {
    themeRows_.clear();
    themeRows_.push_back(ThemeManager::builtinName0);
    themeRows_.push_back(ThemeManager::builtinName1);
    for (const auto& n : ThemeManager::instance().installedNames()) {
        if (n != ThemeManager::builtinName0 && n != ThemeManager::builtinName1)
            themeRows_.push_back(n);
    }
}

std::vector<std::string> App::sessionPaths() const {
    std::vector<std::string> out;
    if (!runtime_) return out;
    for (const auto& p : runtime_->listSessions())
        out.push_back(std::filesystem::path(p).filename().string());
    return out;
}

void App::listMove(int delta) {
    const int count = modal_ == "open"      ? static_cast<int>(sessionPaths().size())
                      : modal_ == "model"   ? static_cast<int>(modelRows_.size())
                      : modal_ == "theme"   ? static_cast<int>(themeRows_.size())
                      : modal_ == "mcp"     ? [&] {
                            mcp::Manager* m = runtime_ ? runtime_->mcpManager() : nullptr;
                            return m ? static_cast<int>(m->serverNames().size()) : 0;
                        }()
                                            : 0;
    if (modal_ == "theme_editor") {
        const int n = static_cast<int>(themeColorFields().size());
        if (n > 0) themeSelect((themeField_ + delta + n) % n);
        return;
    }
    if (count <= 0) return;
    listSelected_ = (listSelected_ + delta + count) % count;
}

void App::listActivate() {
    if (modal_ == "open") {
        const auto paths = runtime_ ? runtime_->listSessions() : std::vector<std::string>{};
        if (listSelected_ >= 0 && listSelected_ < static_cast<int>(paths.size()))
            switchToDialog(paths[static_cast<size_t>(listSelected_)]);
        closeModal();
        return;
    }
    if (modal_ == "model") {
        if (listSelected_ >= 0 && listSelected_ < static_cast<int>(modelRows_.size()) &&
            modelRows_[static_cast<size_t>(listSelected_)] != "(loading models...)")
            setModel(modelRows_[static_cast<size_t>(listSelected_)]);
        closeModal();
        return;
    }
    if (modal_ == "theme") {
        if (listSelected_ >= 0 && listSelected_ < static_cast<int>(themeRows_.size()))
            applyTheme(themeRows_[static_cast<size_t>(listSelected_)]);
        closeModal();
        return;
    }
    if (modal_ == "mcp") {
        if (busy() || !runtime_) return;
        mcp::Manager* m = runtime_->mcpManager();
        if (!m) return;
        const auto names = m->serverNames();
        if (listSelected_ < 0 || listSelected_ >= static_cast<int>(names.size())) return;
        const std::string name = names[static_cast<size_t>(listSelected_)];
        setMcpEnabled(name, !m->enabledOf(name));
    }
}

// ---- theme ---------------------------------------------------------------

void App::themeSelect(int index) {
    if (index < 0 || index >= static_cast<int>(themeColorFields().size())) return;
    // Commit the buffer of the field being left before loading the next one.
    if (themeField_ >= 0 && themeField_ < static_cast<int>(themeColorFields().size()))
        ThemeManager::instance().current();
    themeField_ = index;
    themeHex_ = ThemeManager::instance().current().*(themeColorFields()[static_cast<size_t>(index)].member);
}

void App::themeApply() {
    ThemeColors tc = ThemeManager::instance().current();
    if (themeField_ >= 0 && themeField_ < static_cast<int>(themeColorFields().size()))
        tc.*(themeColorFields()[static_cast<size_t>(themeField_)].member) = themeHex_;
    ThemeManager::instance().applyCustom(tc);
    themeStatus_ = "theme: custom";
}

void App::themeSave() {
    themeApply();
    if (ThemeManager::instance().exportToFile(ThemeManager::instance().current().name))
        themeStatus_ = "theme saved: " + ThemeManager::instance().current().name;
    else
        themeStatus_ = "theme save failed";
}

void App::applyTheme(const std::string& name) {
    if (ThemeManager::instance().switchTo(name)) themeStatus_ = "theme: " + name;
}

// ---- actions -------------------------------------------------------------

void App::send() {
    if (!runtime_) return;
    if (active().input.empty()) return;
    const std::string text = active().input;
    active().input.clear();
    slashOpen_ = false;
    active().followBottom = true;
    if (runtime_->submit(text)) runtime_->startAsync();
}

void App::cancelTurn() {
    if (runtime_) runtime_->cancel();
}

void App::approveTool(int action) {
    if (runtime_) runtime_->approveTool(action);
    modal_ = "none";
}

bool App::approvedAlways() const {
    return runtime_ && runtime_->agent() && runtime_->agent()->canAlways();
}

void App::newChat() {
    if (runtime_) runtime_->newChat();
    active().followBottom = true;
}

void App::switchPromptNext() {
    if (!runtime_ || busy()) return;
    const int n = static_cast<int>(runtime_->promptFiles().size());
    if (n > 0) runtime_->switchPrompt((runtime_->activePromptIndex() + 1) % n);
}

void App::switchPrompt(int index) {
    if (runtime_) runtime_->switchPrompt(index);
}

void App::setModel(const std::string& id) {
    if (runtime_) runtime_->setModel(id);
}

void App::switchToDialog(const std::string& dbPath) {
    if (runtime_) runtime_->switchToDialog(dbPath);
    active().followBottom = true;
}

void App::saveApiKey() {
    if (!runtime_ || form_.apiKey.empty()) return;
    runtime_->saveApiKeyAndConnect(form_.apiKey);
    closeModal();
}

void App::setMcpEnabled(const std::string& name, bool on) {
    if (!runtime_) return;
    refuseIfBusy(&themeStatus_);
    if (busy()) return;
    runtime_->setMcpEnabled(name, on);
}

void App::restartMcp(const std::string& name) {
    if (!runtime_ || busy()) return;
    if (mcp::Manager* m = runtime_->mcpManager()) m->restart(name);
}

void App::refreshModels() {
    if (runtime_) runtime_->refreshModels();
}

void App::toggleReasoning() {
    reasoningExpanded_ = !reasoningExpanded_;
}

void App::toggleDock() {
    dock_ = !dock_;
}

void App::scrollChat(int rows) {
    active().scrollRequest += rows;
    active().followBottom = false;
}

int App::takeScrollRequest() {
    const int n = active().scrollRequest;
    active().scrollRequest = 0;
    return n;
}

bool App::takeFollowBottom() {
    // One-shot: a queued jump is consumed here. Whether the view KEEPS following
    // is decided per frame by how far the reader is from the bottom (drawChat),
    // so a new message no longer yanks the transcript while someone is reading
    // three screens up.
    bool& pending = active().followBottom;
    const bool was = pending;
    pending = false;
    return was;
}

void App::refuseIfBusy(std::string* why) const {
    if (busy() && why) *why = "refused: a turn is running";
}

std::string App::describeState() const {
    return view_model::toJson(active().screen);
}

// ---- the key map (docs/*/user-guide/08-reference.md, key for key) --------

bool App::handleKey(GKey key) {
    if (key != GKey::None) ++debugKeys_;
    const bool anyModal = modal_ != "none";
    if (anyModal) {
        // The TUI's dialogs catch only their own close keys; everything else stays
        // with the dialog. Esc on the approval dialog decides nothing on purpose:
        // a destructive command needs a real answer, not a stray key.
        if (key == GKey::Escape) {
            if (modal_ == "welcome") {
                requestExit(); // no API key, no app: the only way out is Esc
                return true;
            }
            if (modal_ == "approval") return true;
            closeModal();
            return true;
        }
        if (modal_ == "config" && key == GKey::F2) {
            closeModal();
            return true;
        }
        if (modal_ == "help" && key == GKey::F1) {
            closeModal();
            return true;
        }
        if (key == GKey::Up) {
            listMove(-1);
            return true;
        }
        if (key == GKey::Down) {
            listMove(1);
            return true;
        }
        return false;
    }

    switch (key) {
    case GKey::F1:
        openHelpModal();
        return true;
    case GKey::F2:
        openConfigModal();
        return true;
    case GKey::F3:
        newChat();
        return true;
    case GKey::F4:
        openOpenModal();
        return true;
    case GKey::F5:
        openMcpModal();
        return true;
    case GKey::F6:
        openThemeModal();
        return true;
    case GKey::F7:
        openThemeEditorModal();
        return true;
    case GKey::F8:
        toggleReasoning();
        return true;
    case GKey::F9:
        toggleDock();
        return true;
    case GKey::F10:
        openModelModal();
        return true;
    case GKey::Tab:
        switchPromptNext();
        return true;
    case GKey::Escape:
        // Three meanings, decided by state rather than by a modifier key: a
        // running turn is cancelled, a typed line is cleared, and an idle empty
        // prompt asks before closing the app.
        if (slashOpen_) {
            slashClose();
        } else if (busy()) {
            cancelTurn();
        } else if (!active().input.empty()) {
            active().input.clear();
            slashOpen_ = false;
        } else {
            openModal("quit");
        }
        return true;
    case GKey::Up:
        if (slashOpen_) {
            slashMove(-1);
            return true;
        }
        // A prompt taller than one row owns its arrow keys: inside a multiline
        // field Up/Down move the caret, and scrolling the chat at the same time
        // would move two things with one key. Single-row prompts keep the TUI's
        // meaning (the arrow scrolls the transcript).
        if (promptRows_ > 1) return false;
        scrollChat(1);
        return true;
    case GKey::Down:
        if (slashOpen_) {
            slashMove(1);
            return true;
        }
        if (promptRows_ > 1) return false;
        scrollChat(-1);
        return true;
    case GKey::PageUp:
        if (promptRows_ > 1) return false;
        scrollChat(10);
        return true;
    case GKey::PageDown:
        if (promptRows_ > 1) return false;
        scrollChat(-10);
        return true;
    case GKey::None:
    default:
        return false;
    }
}

// ---- config form buffers -------------------------------------------------

std::string* App::configBufferAt(int index) {
    switch (index) {
    case 0:
        return &form_.apiKey;
    case 1:
        return &form_.baseUrl;
    case 2:
        return &form_.model;
    case 3:
        return &form_.maxTokens;
    case 4:
        return &form_.temperature;
    case 6:
        return &form_.contextWindow;
    case 7:
        return &form_.cppCompiler;
    case 8:
        return &form_.python;
    case 9:
        return &form_.clangFormat;
    case 10:
        return &form_.snapGb;
    case 11:
        return &form_.snapDays;
    case 12:
        return &form_.theme;
    default:
        return nullptr;
    }
}

// ---- panes: projection, receipts, injection, sampling --------------------

// One message -> its bubbles, plus the key of each. The key is "<msgId>:<k>",
// msg.id being the database row and k the bubble's position inside THAT message's
// derivation: both survive every streaming append and the oldest-200 trim, which
// a row index does not. A message that is not persisted yet (a notice, id 0) gets
// a per-pane counter instead -- it is appended at the tail and never re-ordered.
void App::appendDerived(Pane& pane, const Message& msg) {
    auto derived = bubble_model::deriveBubbles(msg, pane.knownSpeakers);
    const std::string base =
        msg.id > 0 ? ("m" + std::to_string(msg.id)) : ("n" + std::to_string(++pane.keySeq));
    for (size_t k = 0; k < derived.size(); ++k) {
        pane.chatKeys.push_back(base + ":" + std::to_string(k));
        pane.chat.push_back(std::move(derived[k]));
    }
    if (msg.id > pane.lastMessageId) pane.lastMessageId = msg.id;
    // A bubble was appended: that is the moment to decide whether the view should
    // follow (drawChat checks the distance and whether the reader is moving). Only
    // for the pane on screen -- a background pane's growth must not move this one.
    if (!derived.empty() && &pane == &active()) pane.followBottom = true;
}

void App::refreshChat(Pane& pane) {
    if (!pane.rt || pane.state.load() != Pane::State::Ready || !pane.rt->agent()) return;
    pane.chat.clear();
    pane.chatKeys.clear();
    pane.lastMessageId = 0;
    harvestSpeakers(pane);
    auto msgs = pane.rt->agent()->getNewMessagesSince(0);
    for (const auto& msg : msgs) appendDerived(pane, msg);
}

void App::toggleRowCollapsed(const std::string& key) {
    if (key.empty()) return;
    const auto it = expandedKeys_.find(key);
    if (it == expandedKeys_.end())
        expandedKeys_.insert(key);
    else
        expandedKeys_.erase(it);
}

// A fixed transcript for --demo-chat. It is appended through the SAME path a live
// session uses (appendDerived -> deriveBubbles -> view_model::build), so what a
// screenshot shows is the real derivation, not a mock of it. Five turns, one file
// write with a diff, a long shell result and one failed call.
void App::loadDemoChat() {
    Pane& pane = active();
    demoBusy_ = true; // so the spinner and the running state are on screen
    pane.chat.clear();
    pane.chatKeys.clear();
    pane.lastMessageId = 0;

    int64_t id = 0;
    auto push = [&](Message m) {
        m.id = ++id;
        appendDerived(pane, m);
    };
    auto user = [&](std::string text) { push(Message::User(std::move(text))); };
    auto say = [&](std::string text) { push(Message::Assistant(std::move(text))); };
    auto call = [&](int index, const std::string& name, const std::string& argKey,
                    const std::string& argValue, const std::string& text = std::string()) {
        Message m = Message::Assistant(text);
        ToolCall tc;
        tc.id = "call_" + std::to_string(index);
        tc.name = name;
        tc.arguments = "{\"" + argKey + "\":\"" + argValue + "\"}";
        tc.index = index;
        m.toolCalls.push_back(std::move(tc));
        push(std::move(m));
    };
    auto result = [&](const std::string& name, const std::string& content, const std::string& diff,
                      bool failed) {
        Message m = Message::Tool("call_" + name, name, content, failed);
        m.diff = diff;
        // A real ToolResult carries the wall time the agent measured around the
        // registry call; the demo values are in the same range as the real ones.
        m.durationMs = name == "read_file"     ? 850
                       : name == "grep_files"  ? 240
                       : name == "write_file"  ? 1400
                       : name == "exec_shell"  ? 12400
                                               : 780;
        push(std::move(m));
    };

    user("把 studio 聊天区的工具调用折叠成一行，点开再看完整结果。");
    say("先看合并循环和卡片的绘制位置，确认改动落在哪一层。");
    call(1, "read_file", "path", "src/core/view_model.cpp");
    {
        std::string body;
        for (int i = 0; i < 30; ++i) body += "  // line " + std::to_string(i + 250) + "\n";
        result("read_file", body, "", false);
    }
    call(2, "grep_files", "pattern", "tool_result");
    result("grep_files",
           "D:\\ws\\proJV\\src\\core\\view_model.cpp:311: } else if (b.role == \"tool_call\") {\n"
           "D:\\ws\\proJV\\src\\core\\view_model.cpp:318: while (i + 1 < total && ...)\n"
           "D:\\ws\\proJV\\src\\tui\\chat_view.cpp:224: while (i + 1 < total && ...)\n",
           "", false);
    say("合并循环只有一个落点：`view_model.cpp:318`。diff 会让这个循环提前退出，所以同一次回合里第二个结果会掉出来变成独立卡片。");

    user("把它做出来，然后发我看效果。");
    say("先加结构字段，再把 diff 吸进工具行，最后加点击。");
    call(3, "write_file", "path", "src/core/view_model.cpp");
    result("write_file", "Wrote 421 lines to src/core/view_model.cpp",
           "--- a/src/core/view_model.cpp\n"
           "+++ b/src/core/view_model.cpp\n"
           "@@ -310,5 +318,7 @@\n"
           "     row.kind = \"tool\";\n"
           "-    while (i + 1 < total && bubbles[i + 1].role == \"tool_result\") {\n"
           "-        results += bubbles[i + 1].content;\n"
           "+    while (i + 1 < total) {\n"
           "+        const auto& next = bubbles[i + 1];\n"
           "+        if (next.role == \"tool_result\" || next.role == \"diff\") {\n"
           "+            merge(next);\n"
           "+        } else {\n"
           "+            break;\n"
           "+        }\n",
           false);
    call(4, "exec_shell", "command", "cmake --build build --config Release");
    {
        std::string out;
        for (int i = 0; i < 40; ++i)
            out += "view_model.cpp.obj : warning C4996 deprecated function " + std::to_string(i) +
                   "\n";
        out += "proJV_lib.lib -> build/Release/proJV_lib.lib\n";
        out += "Build succeeded.\n";
        result("exec_shell", out, "", false);
    }
    call(5, "format_file", "path", "src/core/view_model.cpp");
    result("format_file", "path outside the workspace: src/core/view_model.cpp", "", true);
    call(6, "format_file", "path", "find . -name view_model.cpp");
    result("format_file", "formatted src/core/view_model.cpp (3 lines changed)", "", false);
    // One skill run: three roles, two rounds, one dissent. The run id is what a
    // real SkillRunner stamps on every message it writes, and the names have to be
    // in knownSpeakers or the bubble model (rightly) refuses to read "**[X]**" as
    // a role rather than as bold markdown.
    pane.knownSpeakers.insert("Analyst");
    pane.knownSpeakers.insert("Critic");
    pane.knownSpeakers.insert("Writer");
    const std::string run = "skill-1761226800000";
    auto roleMsg = [&](const std::string& role, const std::string& kind, const std::string& text) {
        Message m = Message::Assistant("**[" + role + (kind == "vote" ? " vote" : "") + "]** " + text);
        m.skillRunId = run;
        push(std::move(m));
    };
    roleMsg("Analyst", "message",
            "拆成两层：推导负责合并与折叠状态，前端只负责画和收点击。\n\n"
            "- 合并循环顺带吸收 diff\n"
            "- 折叠 key 由 msg.id 派生，不用行号");
    roleMsg("Analyst", "message", "第二轮补充：`extraFull` 必须存正文，否则展开没有内容可看。");
    roleMsg("Critic", "message",
            "反对同批改 TUI：`chat_view.cpp:224` 和 `view_model.cpp` 是两份独立循环，"
            "一次验收覆盖两个呈现层，出问题分不清是谁的。");
    roleMsg("Critic", "vote", "DISSENT: 这一批不要动 TUI。");
    roleMsg("Writer", "message",
            "验收写成可数的：一屏 4 个回合、折叠行看得出 ok/failed 与改动量、展开不缺行。");

    say("链接通过。折叠默认打开：一屏能看到 4-5 个完整回合，点标题行展开完整结果和 diff。");
    pane.followBottom = true;
}

void App::drainNotices(Pane& pane) {
    std::vector<std::string> notes;
    {
        std::lock_guard<std::mutex> lk(pane.noticeMutex);
        notes.swap(pane.notices);
    }
    if (notes.empty()) return;
    for (auto& n : notes) {
        bubble_model::Bubble cb;
        cb.role = "system";
        cb.content = std::move(n);
        pane.chatKeys.push_back("n" + std::to_string(++pane.keySeq) + ":0");
        pane.chat.push_back(std::move(cb));
    }
    pane.followBottom = true;
}

void App::tick() {
    if (panes_.empty()) return;
    bindActive();
    adoptFinishedPanes();
    // Panes created last frame get their initialize() HERE, on the UI thread, while
    // their cards are already on screen saying "starting...": the click answered
    // immediately, and then the window is busy while the engine builds. One pane per
    // frame keeps a burst of clicks from stalling a single frame for several
    // initialisations at once.
    if (!pendingInit_.empty()) {
        Pane* queued = pendingInit_.front();
        pendingInit_.erase(pendingInit_.begin());
        // The pane may have been closed between the click and this frame; the raw
        // pointer is only dereferenced after its owner is found in panes_.
        const bool alive = queued != nullptr && std::any_of(
            panes_.begin(), panes_.end(),
            [queued](const std::unique_ptr<Pane>& p) { return p.get() == queued; });
        if (alive && queued->state.load() == Pane::State::Starting) {
            Pane& pane = *queued;
            const bool ok = pane.rt && pane.rt->initialize();
            if (ok) pane.workspace = pane.rt->workspace();
            pane.state.store(ok ? Pane::State::Ready : Pane::State::Failed);
            if (!ok) pane.error = "initialize failed (see projv_files/logs)";
            bindActive();
        }
    }
    // Every pane is drained and projected, not only the visible one: a notice or a
    // finished turn must not appear out of order when the user switches back.
    for (auto& owned : panes_) {
        Pane& pane = *owned;
        if (pane.state.load() != Pane::State::Ready) continue;
        drainNotices(pane);
        harvestSpeakers(pane);
        if (Agent* a = pane.rt->agent()) {
            auto newMsgs = a->getNewMessagesSince(pane.lastMessageId);
            for (const auto& msg : newMsgs) appendDerived(pane, msg);
        }
        if (pane.chat.size() > kMaxBubbles) {
            const long drop = static_cast<long>(pane.chat.size() - kMaxBubbles);
            pane.chat.erase(pane.chat.begin(), pane.chat.begin() + drop);
            // The keys are a parallel array: trimming one without the other would
            // hand every remaining row its neighbour's fold key.
            const size_t n = std::min<size_t>(static_cast<size_t>(drop), pane.chatKeys.size());
            pane.chatKeys.erase(pane.chatKeys.begin(), pane.chatKeys.begin() + static_cast<long>(n));
        }
        // The turn clock: started when the pane becomes busy, stopped when it is not.
        const int tokens = pane.rt->promptTokens() + pane.rt->completionTokens();
        const bool busyNow = pane.rt->isBusy();
        if (busyNow) {
            if (!pane.clock.running) work_clock::begin(pane.clock, monotonicMs(), tokens);
        } else if (pane.clock.running) {
            work_clock::end(pane.clock);
        }
        // A turn's END is the falling edge, which is exactly what the TUI beeps on
        // (main_tui.cpp: prevBusy && !curBusy). Every pane can end a turn, so any
        // falling edge asks the platform layer for the same sound.
        if (pane.wasBusy && !busyNow) beepPending_ = true;
        pane.wasBusy = busyNow;
    }

    // An approval request appears and disappears on its own: follow the agent's
    // phase, exactly like the TUI's per-frame sync does. A pane waiting in the
    // background pulls itself forward -- a blocked question must never sit invisible
    // behind another pane.
    if (runtime_ && runtime_->phase() == AgentPhase::AwaitApproval) {
        if (modal_ != "approval") openApprovalModal();
    } else if (modal_ == "approval") {
        modal_ = "none";
    } else {
        for (int i = 0; i < static_cast<int>(panes_.size()); ++i) {
            Pane& pane = *panes_[static_cast<size_t>(i)];
            if (i == active_ || pane.state.load() != Pane::State::Ready) continue;
            if (pane.rt->phase() == AgentPhase::AwaitApproval) {
                activatePane(i);
                openApprovalModal();
                setReceipt("pane " + std::to_string(i + 1) + " is waiting for approval");
                break;
            }
        }
    }

    for (auto& pane : panes_) {
        if (pane->state == Pane::State::Ready) pane->rt->drainPendingQueue();
    }

    // One sampler group per pane: its background tasks, its MCP servers and -- when
    // exactly one pane is running a tool -- the runner's single child, which can only
    // belong to that pane.
    if (stats_) {
        int busy = 0;
        int busyIndex = -1;
        for (int i = 0; i < static_cast<int>(panes_.size()); ++i) {
            if (paneBusy(i)) {
                ++busy;
                busyIndex = i;
            }
        }
        std::vector<TrackedGroup> groups;
        groups.reserve(panes_.size());
        for (int i = 0; i < static_cast<int>(panes_.size()); ++i) {
            Pane& pane = *panes_[static_cast<size_t>(i)];
            TrackedGroup group;
            group.key = i;
            if (pane.state.load() == Pane::State::Ready) {
                for (const auto& task : pane.rt->taskSnapshots()) {
                    if (task.state == bg_task::State::Running && task.pid > 0)
                        group.pids.push_back(task.pid);
                }
                if (mcp::Manager* mcp = pane.rt->mcpManager()) {
                    const std::vector<int> pids = mcp->serverPids();
                    group.pids.insert(group.pids.end(), pids.begin(), pids.end());
                }
                if (busy == 1 && busyIndex == i && runner_) {
                    const int child = runner_->CurrentPid();
                    if (child > 0) group.pids.push_back(child);
                }
            }
            groups.push_back(std::move(group));
        }
        stats_->setTracked(groups);
    }

    fillSource();
    if (active().state.load() == Pane::State::Ready) {
        active().screen = view_model::build(active().source);
    } else {
        // A pane that is starting or has failed has no agent: show its state and
        // nothing else, instead of the previous pane's rows.
        view_model::Source waiting = active().source;
        waiting.phase = AgentPhase::Idle;
        waiting.busy = false;
        waiting.status.state = AgentState::Idle;
        waiting.bubbles.clear();
        waiting.agents.clear();
        waiting.tasks.clear();
        waiting.todoText.clear();
        view_model::Screen scr = view_model::build(waiting);
        scr.statusText = paneStateName(active_);
        scr.statusSpinner = active().state.load() == Pane::State::Starting;
        scr.chat.clear();
        scr.agents.clear();
        scr.agentsEmpty = true;
        scr.tasks.clear();
        scr.tasksEmpty = true;
        scr.todo.clear();
        scr.todoEmpty = true;
        active().screen = std::move(scr);
    }
    spinnerFrame_ = (spinnerFrame_ + 1) % 4;
}

void App::setTreeRoot(const std::string& dir) {
    std::string err;
    if (tree_.setRoot(dir, &err)) {
        treeSelected_ = 0;
        return;
    }
    setReceipt("cannot open " + dir + ": " + err);
}

void App::setLeftSplit(float ratio) {
    if (!(ratio > 0.0f)) return;
    leftSplit_ = std::min(0.8f, std::max(0.2f, ratio));
}

void App::setReceipt(const std::string& text) {
    Pane& pane = active();
    pane.receipt = text;
    pane.receiptUntil = monotonicMs() + 6000; // six seconds, then it stops being true
}

const std::string& App::receipt() const {
    static const std::string empty;
    const Pane& pane = active();
    if (pane.receipt.empty()) return empty;
    if (monotonicMs() > pane.receiptUntil) return empty;
    return pane.receipt;
}

std::string App::paneActivity(int index) const {
    if (index < 0 || index >= static_cast<int>(panes_.size())) return std::string();
    const Pane& pane = *panes_[static_cast<size_t>(index)];
    if (pane.state.load() == Pane::State::Failed) return "failed";
    if (pane.state.load() != Pane::State::Ready || !pane.rt) return "starting...";
    return view_model::phaseName(pane.rt->phase());
}

bool App::takeBeep() {
    const bool pending = beepPending_;
    beepPending_ = false;
    return pending;
}

void App::noteReaderScroll() { active().lastScrollMs = monotonicMs(); }

bool App::readerMovedRecently() const {
    return monotonicMs() - active().lastScrollMs < 400;
}

void App::injectPromptText(const std::string& text) {
    if (text.empty()) return;
    std::string& line = active().input;
    if (!line.empty()) {
        const char last = line.back();
        if (last != ' ' && last != '\n') line += ' ';
    }
    line += text;
    // The field is ImGui-owned while it has the keyboard: it reloads from this
    // buffer only after its active id is cleared (see studio_input.cpp).
    promptReload_ = true;
    setReceipt("sent " + text + " -> prompt");
}

bool App::takePromptReload() {
    const bool wanted = promptReload_;
    promptReload_ = false;
    return wanted;
}

void App::startSampling() {
    if (!stats_) stats_ = CreateSystemStats();
    if (stats_) stats_->start();
}

void App::stopSampling() {
    if (stats_) stats_->stop();
}

StatsSnapshot App::stats() const {
    if (!stats_) return StatsSnapshot{};
    return stats_->snapshot();
}

GroupSample App::paneSample(int index) const {
    const StatsSnapshot snap = stats();
    for (size_t i = 0; i < snap.keys.size() && i < snap.groups.size(); ++i) {
        if (snap.keys[i] == index) return snap.groups[i];
    }
    return GroupSample{};
}

void App::startPaneHere(const std::string& dir) {
    const std::string target = dir.empty() ? getWorkspaceDir() : dir;
    std::error_code ec;
    if (!std::filesystem::is_directory(std::filesystem::path(target), ec) || ec) {
        setReceipt("not a directory: " + target);
        return;
    }
    std::string why;
    if (!newPane(target, &why)) {
        setReceipt("cannot start a pane: " + why);
        return;
    }
    setTreeRoot(target);
}

const ToolPaths& App::toolPaths() const {
    // A static empty ToolPaths allows everything (the documented legacy behaviour),
    // which is exactly right when there is no runtime yet: the tree marks nothing.
    static const ToolPaths empty;
    if (!runtime_) return empty;
    return runtime_->toolPaths();
}

std::string App::workSummary() const {
    if (panes_.empty() || !runtime_) return std::string();
    const Pane& pane = active();
    if (!pane.clock.running) return std::string();
    const long long now = monotonicMs();
    const double seconds = work_clock::elapsedSeconds(pane.clock, now);
    const double rate = work_clock::tokensPerSecond(pane.clock, now, activeTokens());
    char buf[96];
    const AgentStatus status = runtime_->status();
    if (status.state == AgentState::ExecutingTool && status.toolProgressTotal > 0) {
        std::snprintf(buf, sizeof buf, "%.1fs  %.0f tok/s  tool %d/%d", seconds, rate,
                      status.toolProgressCurrent, status.toolProgressTotal);
    } else {
        std::snprintf(buf, sizeof buf, "%.1fs  %.0f tok/s", seconds, rate);
    }
    return buf;
}

int App::activeTokens() const {
    if (!runtime_) return 0;
    return runtime_->promptTokens() + runtime_->completionTokens();
}

long long App::nowMs() const { return monotonicMs(); }

void App::setFollowBottom(bool on) { active().followBottom = on; }

} // namespace studio
