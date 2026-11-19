// proJV TUI -- the view shell (design 08).
//
// The orchestration is gone: it lives in core/SessionRuntime now. This class owns
// the bubbles on screen, the scroll position, the models list for the F10 dialog
// and the theme, and it forwards every action to the runtime. It never calls
// Agent or SkillRunner internals.
//
// Threading: the runtime calls onNotice / onTurnComplete / onStreamingTick from
// its worker thread. Those only wake the render loop or push into a mutex-guarded
// notice queue; the widget tree is touched on the TUI thread only.
#pragma once

#include "models.h"
#include "core/config.h"
#include "core/session_runtime.h"
#include "tui/bubble_model.h"
#include "tui/status_bar.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

class IProcessRunner;
class ToolRegistry;

class TuiApp {
  public:
    TuiApp();
    ~TuiApp();

    bool initialize(IProcessRunner* procRunner);
    bool hasApiKey() const {
        return runtime_ && runtime_->config().loaded && !runtime_->config().apiKey.empty();
    }

    Agent* getAgent() { return runtime_ ? runtime_->agent() : nullptr; }
    mcp::Manager* mcpManager() { return runtime_ ? runtime_->mcpManager() : nullptr; }
    SessionRuntime* runtime() { return runtime_.get(); }

    // Live agent status / phase (forwarded for streaming rendering).
    AgentStatus getStatus() const { return runtime_ ? runtime_->status() : AgentStatus{}; }
    AgentPhase getPhase() const { return runtime_ ? runtime_->phase() : AgentPhase::Idle; }

    // Snapshot of the status-bar inputs.
    status_bar::Data getStatusBarData() const;

    // TODO panel data.
    TodoData copyTodoData() const { return runtime_ ? runtime_->copyTodoData() : TodoData{}; }

    // Config access / persistence / approval.
    const AppConfig& getConfig() const;
    void saveApiKeyAndConnect(const std::string& apiKey);
    void saveFullConfig(const AppConfig& next);
    void approveTool(int action);

    // System prompt role switching.
    const std::vector<std::string>& promptFiles() const;
    int activePromptIndex() const;
    void switchPrompt(int index);

    void setModel(const std::string& model);

    // Session management (new chat / switch dialog).
    void newChat();
    void switchToDialog(const std::string& dbPath);
    std::vector<std::string> listSessions() const;

    void toggleReasoning();
    bool reasoningExpanded() const { return reasoningExpanded_; }

    void emergencyShutdown();

    // Keyboard scroll for the chat area (row-granular, real rendered rows).
    void scrollChat(int delta);
    void resetChatScroll();
    void setChatContentRows(int rows) { chatContentRows_ = rows; }
    void setChatViewportRows(int rows) { chatViewportRows_ = rows; }
    void setChatContentWidth(int cols) { chatContentWidth_ = cols; }
    int chatContentWidth() const { return chatContentWidth_; }
    int chatFocusRow() const;

    // Incremental sync: pull new DB messages into chatHistory (call per frame).
    void syncChatFromAgent();

    const std::vector<bubble_model::Bubble>& bubbles() const { return chatHistory; }

    // Queue a user message and start a turn on a background thread.
    void sendMessage(const std::string& text);
    bool isBusy() const { return runtime_ && runtime_->isBusy(); }
    void cancelTurn();

    void startSkill(const std::string& name, const std::string& topic,
                    const std::string& rawInput = std::string());
    std::string getSkillStatus() const;
    std::vector<SkillRunner::AgentSnapshot> skillAgentStatuses() const;
    size_t pendingCount() const;
    std::vector<std::string> installedSkills() const;
    std::string skillsDir() const;

    std::vector<bg_task::Snapshot> taskSnapshots() const;

    // Invoked on the turn thread when a turn completes (used to trigger a redraw).
    std::function<void()> onTurnComplete;
    std::function<void()> onStreamingTick;
    std::function<void()> onThemeChanged;
    std::function<void()> onModelsUpdated;

    void refreshModels();
    std::vector<ModelInfo> getAvailableModels() const;
    bool modelsLoading() const { return runtime_ && runtime_->modelsLoading(); }

    void shutdown();

    // MCP (F5). Idle only: a process change must never interleave with a turn.
    void setMcpEnabled(const std::string& name, bool enabled);

  private:
    void harvestSpeakers();
    void buildBubblesFromMessages();
    void drainNotices();

    std::unique_ptr<SessionRuntime> runtime_;

    std::vector<bubble_model::Bubble> chatHistory;
    int64_t lastMessageId_ = 0;
    std::set<std::string> knownSpeakers_;
    int chatScrollRow_ = 0;
    bool chatFollowBottom_ = true;
    int chatContentRows_ = 0;
    int chatViewportRows_ = 0;
    int chatContentWidth_ = 0;
    bool reasoningExpanded_ = false;

    // System lines produced on the turn thread. Drained on the TUI thread by
    // syncChatFromAgent, so chatHistory is only ever written from one thread.
    mutable std::mutex noticeMutex_;
    std::vector<std::string> pendingNotices_;
};
