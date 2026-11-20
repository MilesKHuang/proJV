// proJV studio -- the ImGui front-end's state and actions (design 08.5).
//
// Same split as the TUI: SessionRuntime owns the work, this class owns what is on
// screen. It never calls Agent or SkillRunner internals; it submits lines, drives
// the runtime's actions, and projects the result through view_model.
//
// The one rule that keeps the two front-ends honest: nothing here invents text.
// Every label, colour reference and derived string comes back from
// view_model::build(), which is the same derivation the TUI is pinned to.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/session_runtime.h"
#include "core/view_model.h"
#include "models.h"
#include "tui/slash_menu.h"

class IProcessRunner;

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

class App {
  public:
    App();
    ~App();

    bool initialize(IProcessRunner* runner);
    void shutdown();
    void emergencyShutdown();
    bool hasApiKey() const;

    // --- per frame --------------------------------------------------------
    // Pull new messages / notices from the runtime and rebuild the snapshot.
    void tick();
    // Global keys (F1-F10, Tab, Esc, PgUp/PgDn, arrows). Returns true when the
    // key was consumed by the app, false to let ImGui have it.
    bool handleKey(GKey key);
    // Wheel / keyboard chat scroll, in text rows.
    void scrollChat(int rows);
    // Rows requested by the keyboard since the last call (the UI turns them into
    // pixels on the chat child); consumed on read.
    int takeScrollRequest();
    // True once after new content arrived: the chat must jump to the bottom.
    bool takeFollowBottom();
    void setFollowBottom(bool on) { chatFollowBottom_ = on; }

    const view_model::Source& source() const { return source_; }
    const view_model::Screen& screen() const { return screen_; }
    std::string dumpJson() const { return view_model::toJson(screen_); }

    // --- input line -------------------------------------------------------
    const std::string& inputText() const { return input_; }
    void setInputText(const std::string& s);
    // Called once per frame with the edited widget buffer.
    void commitInput(const char* text);
    // Re-open / re-filter the "/" menu after an edit.
    void onInputEdited();
    bool slashOpen() const { return slashOpen_; }
    int slashSelected() const { return slashSelected_; }
    void slashMove(int delta);
    // Click a row in the floating list: selection follows the mouse too.
    void slashSelect(int index);
    // Enter on the menu: fill the line, never send.
    void slashFill();
    // Esc on the menu: close it.
    void slashClose();

    // --- modals -----------------------------------------------------------
    const std::string& modal() const { return modal_; }
    // Diagnostics: how many global keys reached handleKey (probe only).
    int debugKeys() const { return debugKeys_; }
    int debugModalSets() const { return debugModalSets_; }
    void openModal(const std::string& name);
    void closeModal() { modal_ = "none"; }
    ConfigForm& configForm() { return form_; }
    // The text buffer behind one snapshot field index (see fillModalSource's
    // order). Returns nullptr for fields that are not text (radio/check/note).
    std::string* configBufferAt(int index);
    void saveConfigFromForm();
    void clearGrants();
    std::string& themeHex() { return themeHex_; }
    int& themeField() { return themeField_; }
    void themeSelect(int index);
    void themeApply();
    void themeSave();
    int& listSelected() { return listSelected_; }
    void listActivate(); // Enter on the open/model/theme list

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

    // Headless snapshot of one scripted state, for `--dump-view-model`.
    std::string describeState() const;

  private:
    void fillSource();
    void fillModalSource(view_model::Source& s);
    void listMove(int delta);
    void refreshChat();
    void drainNotices();
    void harvestSpeakers();
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

    std::unique_ptr<SessionRuntime> runtime_;

    // chat projection (mirrors TuiApp: one writer, the studio thread)
    std::vector<bubble_model::Bubble> chat_;
    int64_t lastMessageId_ = 0;
    std::set<std::string> knownSpeakers_;
    mutable std::mutex noticeMutex_;
    std::vector<std::string> pendingNotices_;
    int chatScrollRows_ = 0;
    bool chatFollowBottom_ = true;
    int scrollRequest_ = 0;
    view_model::Source source_;
    view_model::Screen screen_;

    std::string input_;
    std::string modal_ = "none";
    bool slashOpen_ = false;
    int slashSelected_ = 0;
    std::vector<help_text::CmdHint> slashAll_;
    std::vector<help_text::CmdHint> slashRows_;

    ConfigForm form_;
    std::string themeHex_;
    int themeField_ = 0;
    std::string themeStatus_;
    int listSelected_ = 0;
    std::vector<std::string> listPaths_;
    std::vector<std::string> modelRows_;
    std::vector<std::string> themeRows_;
    int spinnerFrame_ = 0;
    bool reasoningExpanded_ = false;
    bool dock_ = true;
    bool wantExit_ = false;
    bool bannerShown_ = false;
    float dpiScale_ = 1.0f;
    int debugKeys_ = 0;
    int debugModalSets_ = 0;};

} // namespace studio
