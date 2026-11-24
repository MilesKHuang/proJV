// proJV studio -- the ImGui front-end's state and actions (design 08.5).
//
// Same split as the TUI: SessionRuntime owns the work, this class owns what is on
// screen. It never calls Agent or SkillRunner internals; it submits lines, drives
// the runtime's actions, and projects the result through view_model.
//
// The one rule that keeps the two front-ends honest: nothing here invents text.
// Every label, colour reference and derived string about a conversation comes back
// from view_model::build(), which is the same derivation the TUI is pinned to.
//
// Since the left column landed, a window can own SEVERAL panes. A pane is one
// SessionRuntime plus the state that belongs to it alone (its chat, its prompt
// line, its derived Screen); the chrome (header, footer, modals, the file tree, the
// resource gauges) is shared and reads the ACTIVE pane. `runtime_` is a cached
// pointer to the active pane's runtime, which is why the turn-driving code still
// reads like the single-pane version it grew out of.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/config.h"
#include "core/session_runtime.h"
#include "core/view_model.h"
#include "design/file_tree.h"
#include "design/work_clock.h"
#include "models.h"
#include "tui/slash_menu.h"

struct IProcessRunner;
class ISystemStats;
struct StatsSnapshot;
struct GroupSample;

namespace studio {

// Keys the app itself owns. Everything else (text, Enter, mouse) belongs to the
// widget under the cursor, so the platform layer maps only these and hands them
// over before any widget sees them -- the same order the TUI uses, where the
// input component's CatchEvent runs before the Input widget.
enum class GKey {
    None,
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    Tab,
    Escape,
    Up,
    Down,
    PageUp,
    PageDown,
};

// The config dialog edits a COPY, exactly like tui/config_view.cpp: nothing is
// written until Save, so a half-typed number can never reach config.toml.
struct ConfigForm {
    AppConfig cfg;
    std::string apiKey;
    std::string baseUrl;
    std::string model;
    std::string maxTokens;
    std::string temperature;
    std::string contextWindow;
    std::string cppCompiler;
    std::string python;
    std::string clangFormat;
    std::string snapGb;
    std::string snapDays;
    std::string theme;
    int effortIndex = 2;
    int modeIndex = 1;
    bool osc = true;
};

// One owned session: its runtime, and everything about it that is not shared.
struct Pane {
    std::unique_ptr<SessionRuntime> rt;
    std::vector<bubble_model::Bubble> chat;
    int64_t lastMessageId = 0;
    std::set<std::string> knownSpeakers;
    std::string input;
    std::string title;     // "proJV @ projV"
    std::string workspace; // the directory its tools resolve against
    view_model::Source source;
    view_model::Screen screen;
    bool followBottom = true;
    int scrollRequest = 0;
    // A pane is initialised on the UI thread (see App::newPane); there is no
    // startup thread, so there is nothing to join and no hand-off to order.
    enum class State { Starting, Ready, Failed };
    std::atomic<State> state{State::Starting};
    bool adopted = false; // tick() has seen this pane become ready
    std::string error;
    work_clock::Clock clock;
    int tokensMark = 0; // prompt+completion when the current turn started
    // Notices arrive on the TURN thread of this pane: queued here, drained by the
    // UI thread on its next tick. One queue per pane, so a message from a pane the
    // user is not looking at cannot land in another pane's chat.
    std::mutex noticeMutex;
    std::vector<std::string> notices;
};

class App {
  public:
    App();
    ~App();

    bool initialize(IProcessRunner* runner);
    void shutdown();
    void emergencyShutdown();
    bool hasApiKey() const;

    // --- per frame --------------------------------------------------------
    // Pull new messages / notices from every runtime and rebuild the snapshot of
    // the active pane.
    void tick();
    // Global keys (F1-F10, Tab, Esc, PgUp/PgDn, arrows). Returns true when the
    // key was consumed by the app, false to let ImGui have it.
    bool handleKey(GKey key);
    void scrollChat(int rows);
    // Rows requested by the keyboard since the last call (consumed on read).
    int takeScrollRequest();
    // True once after new content arrived: the chat must jump to the bottom.
    bool takeFollowBottom();
    void setFollowBottom(bool on);

    const view_model::Source& source() const { return active().source; }
    const view_model::Screen& screen() const { return active().screen; }
    std::string dumpJson() const { return view_model::toJson(active().screen); }

    // --- panes ------------------------------------------------------------
    int paneCount() const { return static_cast<int>(panes_.size()); }
    int activePane() const { return active_; }
    bool paneBusy(int index) const;
    const std::string& paneTitle(int index) const;
    const std::string& paneWorkspace(int index) const;
    const view_model::Screen& screenOf(int index) const;
 // ready / starting... / failed -- one word for the card's state column.
 const std::string& paneStateName(int index) const;
 bool paneReady(int index) const;
    void activatePane(int index);
    // False when the pane is running a turn: closing it would orphan the worker.
    bool closePane(int index);
    // Create a pane whose tools resolve against `workspace` ("" = launch dir).
    // `syncNow` = do the (3-5 s) initialize right here: used for the first pane,
    // before the window exists. Later panes queue it for the next tick, so the card
    // is visible first.
    bool newPane(const std::string& workspace, std::string* why, bool syncNow = false);

    // --- input line -------------------------------------------------------
    const std::string& inputText() const { return active().input; }
    void setInputText(const std::string& s);
    // Visual rows the composer holds this frame (set by the prompt widget).
    void setPromptRows(int rows) { promptRows_ = rows > 0 ? rows : 1; }
    int promptRows() const { return promptRows_; }
    void commitInput(const char* text);
    void onInputEdited();
    bool slashOpen() const { return slashOpen_; }
    int slashSelected() const { return slashSelected_; }
    void slashMove(int delta);
    void slashSelect(int index);
    void slashFill();
    void slashClose();

    // A path or any other text sent from the left column. Goes through the same
    // buffer comparison the slash fill uses: while the field owns the keyboard,
    // only a reload makes it show the new text (see studio_input.cpp).
    void injectPromptText(const std::string& text);
    bool takePromptReload();

    // --- the left column --------------------------------------------------
    ftree::Tree& tree() { return tree_; }
    // The roots the agent may touch: the tree marks rows outside them.
    const ToolPaths& toolPaths() const;
    void setTreeRoot(const std::string& dir);
    int treeSelected() const { return treeSelected_; }
    void setTreeSelected(int index) { treeSelected_ = index; }
    bool leftOpen() const { return leftOpen_; }
    void setLeftOpen(bool open) { leftOpen_ = open; }
    float leftSplit() const { return leftSplit_; }
    void setLeftSplit(float ratio);
    // One line saying what just happened, so a click never has a silent result.
    void setReceipt(const std::string& text);
    const std::string& receipt() const;
    // Resource sampling (started by the window's main loop, never headless). By
    // value: the sampler copies its last snapshot under a lock, and a reference
    // into that would go stale the moment the thread took the next sample.
    void startSampling();
    void stopSampling();
    StatsSnapshot stats() const;

    // Right-click targets. One popup per frame is drawn for these, after the rows:
    // a popup opened and queried once per row is an ImGui id conflict.
    int contextPane() const { return contextPane_; }
    void setContextPane(int index) { contextPane_ = index; }
    const std::string& contextPath() const { return contextPath_; }
    bool contextIsDir() const { return contextIsDir_; }
    void setContextPath(std::string path, bool isDir) { contextPath_ = std::move(path); contextIsDir_ = isDir; }
    void clearContextPath() { contextPath_.clear(); }
    // Start a pane whose tools resolve against `dir`. This is the ONLY way in
    // (the reference panel works the same way): the directory comes from the file
    // tree -- its root or a row you right-clicked -- never from a typed dialog.
    void startPaneHere(const std::string& dir);
    // The last sampled CPU/MEM/GPU of one pane (key = pane index).
    GroupSample paneSample(int index) const;

    // --- modals -----------------------------------------------------------
    const std::string& modal() const { return modal_; }
    int debugKeys() const { return debugKeys_; }
    int debugModalSets() const { return debugModalSets_; }
    void openModal(const std::string& name);
    void closeModal() { modal_ = "none"; }
    ConfigForm& configForm() { return form_; }
    std::string* configBufferAt(int index);
    void saveConfigFromForm();
    void clearGrants();
    std::string& themeHex() { return themeHex_; }
    int& themeField() { return themeField_; }
    void themeSelect(int index);
    void themeApply();
    void themeSave();
    int& listSelected() { return listSelected_; }
    void listActivate();

    // --- actions ----------------------------------------------------------
    void send();
    void cancelTurn();
    void approveTool(int action);
    void newChat();
    void switchPromptNext();
    void switchPrompt(int index);
    void setModel(const std::string& id);
    void switchToDialog(const std::string& dbPath);
    void saveApiKey();
    void setMcpEnabled(const std::string& name, bool on);
    void restartMcp(const std::string& name);
    void refreshModels();
    void applyTheme(const std::string& name);
    void toggleReasoning();
    void toggleDock();
    void beginQuit() { openModal("quit"); }
    void requestExit() { wantExit_ = true; }
    bool wantExit() const { return wantExit_; }
    void refuseIfBusy(std::string* why) const;

    bool busy() const { return runtime_ && runtime_->isBusy(); }
    bool approvedAlways() const;
    const std::string& themeStatus() const { return themeStatus_; }
    float dpiScale() const { return dpiScale_; }
    void setDpiScale(float s) { dpiScale_ = s; }

    std::string describeState() const;

    // "12.4s - 34 tok/s - tool 2/3" for the active pane, or "" when it is idle.
    // One derivation, so the status line and the System panel cannot disagree. The
    // view model never reads a clock (its snapshots are golden tests), which is why
    // this lives here.
    std::string workSummary() const;
    // Prompt+completion tokens of the active pane, for the throughput figure.
    int activeTokens() const;
    long long nowMs() const;

  private:
    Pane& active();
    const Pane& active() const;
    void bindActive();
    void wireRuntime(Pane& pane);
 // Called by tick(): a pane whose startup thread finished becomes usable here,
 // on the UI thread (theme init, command table, welcome dialog on the first one).
 void adoptFinishedPanes();
 // Two panes in folders with the same name would read identically; the second one
 // gets a suffix. Recomputed whenever the set of panes changes.
 void refreshPaneTitles();
    void fillSource();
    void fillModalSource(view_model::Source& s);
    void listMove(int delta);
    void refreshChat(Pane& pane);
    void drainNotices(Pane& pane);
    void harvestSpeakers(Pane& pane);
    float sourceCost() const;
    void refreshModelRows();
    void refreshThemeRows();
    void openConfigModal();
    void openWelcomeModal();
    void openApprovalModal();
    void openMcpModal();
    void openOpenModal();
    void openModelModal();
    void openThemeModal();
    void openThemeEditorModal();
    void openHelpModal();
    std::vector<std::string> sessionPaths() const;

    // The active pane's runtime, cached by bindActive() so the turn-driving code
    // reads exactly like the single-pane version it grew out of.
    SessionRuntime* runtime_ = nullptr;
    std::vector<std::unique_ptr<Pane>> panes_;
    int active_ = 0;
    IProcessRunner* runner_ = nullptr;

    // Shared chrome state.
    std::string modal_ = "none";
    bool slashOpen_ = false;
    int slashSelected_ = 0;
    std::vector<help_text::CmdHint> slashAll_;
    std::vector<help_text::CmdHint> slashRows_;
    int promptRows_ = 1;
    bool promptReload_ = false;
    int spinnerFrame_ = 0;
    bool reasoningExpanded_ = false;
    bool dock_ = true;
    bool wantExit_ = false;
    bool bannerShown_ = false;
    float dpiScale_ = 1.0f;
    int debugKeys_ = 0;
    int debugModalSets_ = 0;
    // One-time startup: the first pane to become ready initialises the theme, the
    // "/" command table and the welcome dialog, because those used to run inside
    // App::initialize() on the UI thread (which is now off it).
    bool finishStartup_ = false;
    bool firstPaneReady_ = false;
    // Panes created this frame, waiting for their initialize() on the next tick.
    // A queue, not a single slot: two clicks in one frame must both be initialised.
    // It holds the Pane*, not its index: closing any pane shifts every index after
    // it, and a queued index would then point at a different session.
    std::vector<Pane*> pendingInit_;

    // The left column.
    ftree::Tree tree_;
    int treeSelected_ = 0;
    bool leftOpen_ = true;
    float leftSplit_ = 0.55f; // share of the column the session list gets
    std::string receipt_;
    long long receiptUntilMs_ = 0;
    int contextPane_ = -1;
    std::string contextPath_;
    bool contextIsDir_ = false;


    ConfigForm form_;
    std::string themeHex_;
    int themeField_ = 0;
    std::string themeStatus_;
    int listSelected_ = 0;
    std::vector<std::string> listPaths_;
    std::vector<std::string> modelRows_;
    std::vector<std::string> themeRows_;

    ISystemStats* stats_ = nullptr;
};

} // namespace studio
