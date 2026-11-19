// proJV TUI -- the view shell (design 08). Everything that is not drawing lives
// in core/SessionRuntime; this file only forwards, plus the bubble list.
#include "app_tui.h"

#include "core/config.h"
#include "core/terminal_status.h"
#include "tui/theme_manager.h"
#include "debug_log.h"

#include <algorithm>
#include <ctime>

namespace {
// Empty fallbacks so a call before initialize() can never dereference null.
const AppConfig& emptyConfig() {
    static const AppConfig empty;
    return empty;
}
const std::vector<std::string>& emptyStrings() {
    static const std::vector<std::string> empty;
    return empty;
}
} // namespace

TuiApp::TuiApp() = default;

TuiApp::~TuiApp() {
    shutdown();
}

bool TuiApp::initialize(IProcessRunner* procRunner) {
    runtime_ = std::make_unique<SessionRuntime>();
    runtime_->setProcessRunner(procRunner);

    // The runtime reports; the shell paints. No widget is touched from here:
    // every callback below runs on the turn thread.
    runtime_->onNotice = [this](const std::string& text) {
        {
            std::lock_guard<std::mutex> lk(noticeMutex_);
            pendingNotices_.push_back(text);
        }
        if (onStreamingTick) onStreamingTick(); // wake the render loop
    };
    runtime_->onTurnComplete = [this] {
        if (onTurnComplete) onTurnComplete();
    };
    runtime_->onStreamingTick = [this] {
        if (onStreamingTick) onStreamingTick();
    };
    runtime_->onModelsUpdated = [this] {
        if (onModelsUpdated) onModelsUpdated();
    };
    runtime_->onSessionReset = [this] {
        buildBubblesFromMessages();
        resetChatScroll();
    };
    // OSC 7501 is a terminal feature of this front-end, and 08 keeps it here: the
    // runtime does not know whether a host is watching.
    runtime_->onPhase = [](AgentPhase p, const std::string& msg) {
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
    };

    terminal_status::init(getConfig().oscStatus);
    if (!runtime_->initialize()) return false;

    // Theme: load from projv_files/theme/, prefer config.themeName.
    ThemeManager::instance().init(getThemeDir(), getConfig().themeName);
    ThemeManager::instance().onThemeChanged = [this](const std::string& name) {
        if (!runtime_) return;
        runtime_->config().themeName = name;
        saveConfig(runtime_->config());
        if (onThemeChanged) onThemeChanged();
    };

    buildBubblesFromMessages();
    refreshModels(); // kick off model list fetch (async)
    return true;
}

// ---- view state ---------------------------------------------------------

void TuiApp::harvestSpeakers() {
    // The active skill's role display names are the registry of known speakers.
    // Harvested each frame (cheap, mutex-guarded) so it tracks skills as they
    // start; entries persist for the rest of the app run.
    for (const auto& a : skillAgentStatuses()) {
        if (!a.displayName.empty()) knownSpeakers_.insert(a.displayName);
    }
}

void TuiApp::buildBubblesFromMessages() {
    if (!runtime_ || !runtime_->agent()) return;
    chatHistory.clear();
    lastMessageId_ = 0;
    harvestSpeakers();

    auto msgs = runtime_->agent()->getNewMessagesSince(0);
    for (const auto& msg : msgs) {
        auto derived = bubble_model::deriveBubbles(msg, knownSpeakers_);
        chatHistory.insert(chatHistory.end(), derived.begin(), derived.end());
        if (msg.id > lastMessageId_) lastMessageId_ = msg.id;
    }
}

void TuiApp::drainNotices() {
    std::vector<std::string> notes;
    {
        std::lock_guard<std::mutex> lk(noticeMutex_);
        notes.swap(pendingNotices_);
    }
    if (notes.empty()) return;
    for (auto& n : notes) {
        bubble_model::Bubble cb;
        cb.role = "system";
        cb.content = std::move(n);
        chatHistory.push_back(std::move(cb));
    }
    resetChatScroll();
}

void TuiApp::syncChatFromAgent() {
    if (!runtime_ || !runtime_->agent()) return;
    drainNotices();
    harvestSpeakers();

    auto newMsgs = runtime_->agent()->getNewMessagesSince(lastMessageId_);
    if (!newMsgs.empty()) resetChatScroll(); // new content -> scroll back to bottom
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
    if (runtime_) runtime_->drainPendingQueue();
}

status_bar::Data TuiApp::getStatusBarData() const {
    status_bar::Data d;
    if (!runtime_) return d;
    const AppConfig& cfg = runtime_->config();
    d.model = cfg.model;
    d.promptTokens = runtime_->promptTokens();
    d.completionTokens = runtime_->completionTokens();
    d.status = getStatus();
    d.workspace = getWorkspaceDir();
    d.cost = d.promptTokens * lookupModelPrice(cfg.modelPrices, cfg.model, true) * 1e-6 +
             d.completionTokens * lookupModelPrice(cfg.modelPrices, cfg.model, false) * 1e-6;
    const int idx = runtime_->activePromptIndex();
    const auto& files = runtime_->promptFiles();
    if (idx >= 0 && idx < static_cast<int>(files.size())) {
        d.roleName = files[static_cast<size_t>(idx)];
        if (d.roleName.size() > 3 && d.roleName.substr(d.roleName.size() - 3) == ".md")
            d.roleName = d.roleName.substr(0, d.roleName.size() - 3);
    }
    if (const Agent* a = runtime_->agent()) {
        d.msgCount = a->getSession().messageCount();
        int toolCount = 0;
        for (const auto& m : a->getSession().getContextMessages()) {
            if (m.role == "tool") ++toolCount;
        }
        d.toolCount = toolCount;
        d.pressure = a->getContextPressure();
        d.estimatedTokens = a->getEstimatedContextTokens();
        d.windowTokens = a->getContextWindowSize();
    }
    return d;
}

// ---- forwarded actions --------------------------------------------------

const AppConfig& TuiApp::getConfig() const {
    return runtime_ ? runtime_->config() : emptyConfig();
}

void TuiApp::saveApiKeyAndConnect(const std::string& apiKey) {
    if (runtime_) runtime_->saveApiKeyAndConnect(apiKey);
}

void TuiApp::saveFullConfig(const AppConfig& next) {
    if (runtime_) runtime_->saveFullConfig(next);
}

void TuiApp::approveTool(int action) {
    if (runtime_) runtime_->approveTool(action);
}

const std::vector<std::string>& TuiApp::promptFiles() const {
    return runtime_ ? runtime_->promptFiles() : emptyStrings();
}

int TuiApp::activePromptIndex() const {
    return runtime_ ? runtime_->activePromptIndex() : 0;
}

void TuiApp::switchPrompt(int index) {
    if (runtime_) runtime_->switchPrompt(index);
}

void TuiApp::setModel(const std::string& model) {
    if (runtime_) runtime_->setModel(model);
}

void TuiApp::newChat() {
    if (runtime_) runtime_->newChat();
}

void TuiApp::switchToDialog(const std::string& dbPath) {
    if (runtime_) runtime_->switchToDialog(dbPath);
}

std::vector<std::string> TuiApp::listSessions() const {
    return runtime_ ? runtime_->listSessions() : std::vector<std::string>{};
}

void TuiApp::sendMessage(const std::string& text) {
    if (!runtime_) return;
    if (runtime_->submit(text)) runtime_->startAsync();
}

void TuiApp::cancelTurn() {
    if (runtime_) runtime_->cancel();
}

void TuiApp::startSkill(const std::string& name, const std::string& topic,
                        const std::string& rawInput) {
    if (runtime_) runtime_->startSkill(name, topic, rawInput);
}

std::string TuiApp::getSkillStatus() const {
    return runtime_ ? runtime_->skillStatus() : std::string();
}

std::vector<SkillRunner::AgentSnapshot> TuiApp::skillAgentStatuses() const {
    return runtime_ ? runtime_->skillAgentStatuses() : std::vector<SkillRunner::AgentSnapshot>{};
}

size_t TuiApp::pendingCount() const {
    return runtime_ ? runtime_->pendingCount() : 0;
}

std::vector<std::string> TuiApp::installedSkills() const {
    return runtime_ ? runtime_->installedSkills() : std::vector<std::string>{};
}

std::string TuiApp::skillsDir() const {
    return runtime_ ? runtime_->skillsDir() : std::string();
}

std::vector<bg_task::Snapshot> TuiApp::taskSnapshots() const {
    return runtime_ ? runtime_->taskSnapshots() : std::vector<bg_task::Snapshot>{};
}

void TuiApp::refreshModels() {
    if (runtime_) runtime_->refreshModels();
}

std::vector<ModelInfo> TuiApp::getAvailableModels() const {
    return runtime_ ? runtime_->getAvailableModels() : std::vector<ModelInfo>{};
}

void TuiApp::setMcpEnabled(const std::string& name, bool enabled) {
    if (runtime_) runtime_->setMcpEnabled(name, enabled);
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

void TuiApp::shutdown() {
    if (!runtime_) return;
    runtime_->shutdown();
    runtime_.reset();
    // Drop the host-side status record so no zombie "working" stays behind.
    terminal_status::shutdown();
}

void TuiApp::emergencyShutdown() {
    if (!runtime_) return;
    runtime_->emergencyShutdown();
}
