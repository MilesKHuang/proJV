// proJV studio -- the front-end's state and actions (see studio_app.h).
#include "studio_app.h"

#include "core/help_text.h"
#include "core/snapshot.h"
#include "core/speaker_slots.h"
#include "platform/isystem_util.h"
#include "tui/approval_logic.h"
#include "tui/paste_flatten.h"
#include "tui/theme_manager.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace studio {
namespace {

constexpr size_t kMaxBubbles = 200; // bound the rendering cost, same cap as TuiApp

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

} // namespace

App::App() = default;
App::~App() = default;

bool App::initialize(IProcessRunner* runner) {
    runtime_ = std::make_unique<SessionRuntime>();
    runtime_->setProcessRunner(runner);

    // Every callback below runs on the TURN thread. None of them touches a widget:
    // they queue a string or set a flag, and tick() reads it on the studio thread.
    runtime_->onNotice = [this](const std::string& text) {
        std::lock_guard<std::mutex> lk(noticeMutex_);
        pendingNotices_.push_back(text);
    };
    runtime_->onSessionReset = [this] {
        chat_.clear();
        lastMessageId_ = 0;
        chatFollowBottom_ = true;
    };
    // OSC 7501 is a terminal feature. A window has no host terminal to report to,
    // so the studio leaves the phase channel unset on purpose.
    if (!runtime_->initialize()) return false;

    ThemeManager::instance().init(getThemeDir(), runtime_->config().themeName);
    ThemeManager::instance().onThemeChanged = [this](const std::string& name) {
        if (!runtime_) return;
        runtime_->config().themeName = name;
        saveConfig(runtime_->config());
        themeStatus_ = "theme: " + name;
    };

    slashAll_ = slash_menu::all(runtime_->installedSkills(), runtime_->skillsDir());

    refreshChat();
    refreshModels();
    if (!hasApiKey()) openWelcomeModal();
    return true;
}

void App::shutdown() {
    if (runtime_) runtime_->shutdown();
}

void App::emergencyShutdown() {
    if (runtime_) runtime_->emergencyShutdown();
}

bool App::hasApiKey() const {
    return runtime_ && runtime_->config().loaded && !runtime_->config().apiKey.empty();
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

void App::harvestSpeakers() {
    if (!runtime_) return;
    for (const auto& a : runtime_->skillAgentStatuses()) {
        if (!a.displayName.empty()) knownSpeakers_.insert(a.displayName);
    }
}

void App::refreshChat() {
    if (!runtime_ || !runtime_->agent()) return;
    chat_.clear();
    lastMessageId_ = 0;
    harvestSpeakers();
    auto msgs = runtime_->agent()->getNewMessagesSince(0);
    for (const auto& msg : msgs) {
        auto derived = bubble_model::deriveBubbles(msg, knownSpeakers_);
        chat_.insert(chat_.end(), derived.begin(), derived.end());
        if (msg.id > lastMessageId_) lastMessageId_ = msg.id;
    }
}

void App::drainNotices() {
    std::vector<std::string> notes;
    {
        std::lock_guard<std::mutex> lk(noticeMutex_);
        notes.swap(pendingNotices_);
    }
    for (auto& n : notes) {
        bubble_model::Bubble cb;
        cb.role = "system";
        cb.content = std::move(n);
        chat_.push_back(std::move(cb));
    }
    if (!notes.empty()) chatFollowBottom_ = true;
}

void App::tick() {
    if (!runtime_) return;
    drainNotices();
    harvestSpeakers();

    if (Agent* a = runtime_->agent()) {
        auto newMsgs = a->getNewMessagesSince(lastMessageId_);
        for (const auto& msg : newMsgs) {
            auto derived = bubble_model::deriveBubbles(msg, knownSpeakers_);
            chat_.insert(chat_.end(), derived.begin(), derived.end());
            if (msg.id > lastMessageId_) lastMessageId_ = msg.id;
        }
        // No forced "follow the bottom" here: the shell owns that flag and writes it
        // every frame from the real scroll position. Setting it true from the engine
        // side is what yanked the transcript back down while it was being read.
    }
    if (chat_.size() > kMaxBubbles) {
        chat_.erase(chat_.begin(), chat_.begin() + static_cast<long>(chat_.size() - kMaxBubbles));
    }

    // An approval request appears and disappears on its own: follow the agent's
    // phase, exactly like the TUI's per-frame sync does.
    if (runtime_->phase() == AgentPhase::AwaitApproval) {
        if (modal_ != "approval") openApprovalModal();
    } else if (modal_ == "approval") {
        modal_ = "none";
    }

    runtime_->drainPendingQueue();
    fillSource();
    screen_ = view_model::build(source_);
    spinnerFrame_ = (spinnerFrame_ + 1) % 4;
}

void App::fillSource() {
    view_model::Source s;
    s.version = PROJV_VERSION;
    s.modal = modal_;
    s.dock = dock_;
    s.phase = runtime_ ? runtime_->phase() : AgentPhase::Idle;
    s.busy = busy();
    s.spinnerFrame = spinnerFrame_;
    s.model = runtime_ ? runtime_->config().model : std::string();
    s.workspace = getWorkspaceDir();
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
    s.bubbles = chat_;
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

    s.input.text = input_;
    s.input.cursor = static_cast<int>(input_.size());
    s.input.placeholder = "type a message (Enter=send, Esc=quit?, F2=config)";

    fillModalSource(s);
    source_ = std::move(s);
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
            snapshot::isAvailable()
                ? "[Undo available] the files this turn changes can be restored with /undo"
                : "[Undo unavailable] a deletion here cannot be reverted";
        s.approval.buttons = {"YES -- Execute Once", "ALWAYS -- Allow This Command",
                              "NO -- Cancel"};
    }
}

// ---- input line ----------------------------------------------------------

void App::setInputText(const std::string& s) {
    input_ = s;
    onInputEdited();
}

void App::commitInput(const char* text) {
    if (!text) return;
    std::string next = text;
    // The TUI flattens a paste: line breaks and tabs become spaces (a pasted
    // block must never send itself) and an absurd paste is capped. ImGui already
    // strips newlines from a single-line field; tabs and the cap are ours.
    bool changed = false;
    for (char& c : next) {
        if (c == '\t' || c == '\r' || c == '\n') {
            c = ' ';
            changed = true;
        }
    }
    if (next.size() > paste_flatten::kMaxFlattenChars) {
        next.resize(paste_flatten::kMaxFlattenChars);
        changed = true;
    }
    (void)changed;
    if (next == input_) return;
    input_ = std::move(next);
    onInputEdited();
}

void App::onInputEdited() {
    // Only a leading "/" with no space yet can be a command name. Typing the
    // first character of the arguments ("/wish a...") closes the menu, which is
    // exactly what the user means.
    const size_t sp = input_.find_first_of(" \t");
    const bool bare = !input_.empty() && input_[0] == '/' && sp == std::string::npos;
    if (!bare) {
        slashOpen_ = false;
        return;
    }
    slashRows_ = slash_menu::filter(slashAll_, input_);
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
    input_ = slash_menu::completion(slashRows_[static_cast<size_t>(slashSelected_)]);
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
    if (input_.empty()) return;
    const std::string text = input_;
    input_.clear();
    slashOpen_ = false;
    chatFollowBottom_ = true;
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
    chatFollowBottom_ = true;
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
    chatFollowBottom_ = true;
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
    scrollRequest_ += rows;
    chatFollowBottom_ = false;
}

int App::takeScrollRequest() {
    const int n = scrollRequest_;
    scrollRequest_ = 0;
    return n;
}

bool App::takeFollowBottom() {
    const bool follow = chatFollowBottom_;
    return follow;
}

void App::refuseIfBusy(std::string* why) const {
    if (busy() && why) *why = "refused: a turn is running";
}

std::string App::describeState() const {
    return view_model::toJson(screen_);
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
        } else if (!input_.empty()) {
            input_.clear();
            slashOpen_ = false;
        } else {
            openModal("quit");
        }
        return true;
    case GKey::Up:
        if (slashOpen_)
            slashMove(-1);
        else
            scrollChat(1);
        return true;
    case GKey::Down:
        if (slashOpen_)
            slashMove(1);
        else
            scrollChat(-1);
        return true;
    case GKey::PageUp:
        scrollChat(10);
        return true;
    case GKey::PageDown:
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

} // namespace studio
