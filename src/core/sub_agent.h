#pragma once
#include "models.h"
#include "core/agent.h"
#include "client/deepseek.h"
#include "tools/registry.h"
#include <memory>
#include <thread>
#include <future>
#include <atomic>
#include <mutex>
#include <string>

enum class SubAgentState {
    Idle,
    Running,
    Completed,
    Failed,
    Cancelled
};

struct SubAgentResult {
    std::string agentId;
    std::string finalReply;
    int toolCalls = 0;
    int64_t elapsedMs = 0;
    bool success = false;
    std::string errorMessage;
};

struct SubAgentStatus {
    std::string agentId;
    std::string agentType;
    SubAgentState state = SubAgentState::Idle;
    AgentPhase phase = AgentPhase::Idle;
    std::string currentTool;
    int toolProgressCurrent = 0;
    int toolProgressTotal = 0;
    std::string taskSummary;
    std::string lastReply;     // most recent non-tool-call assistant text
    int64_t elapsedMs = 0;
    int idleRounds = 0;
};

class SubAgentManager; // forward

class SubAgent {
    friend class SubAgentManager;
public:
    SubAgent(const std::string& agentId,
             const std::string& agentType,
             const std::string& model,
             const std::string& workspacePath,
             std::shared_ptr<std::atomic<bool>> cancelFlag);
    ~SubAgent();

    void cancel();
    SubAgentStatus getStatus() const;
    SubAgentResult getResult() const;
    void wait();

    const std::string& agentId() const { return agentId_; }
    const std::string& agentType() const { return agentType_; }
    SubAgentState state() const { return state_.load(); }
    bool isDone() const {
        auto s = state_.load();
        return s == SubAgentState::Completed || s == SubAgentState::Failed || s == SubAgentState::Cancelled;
    }
    bool isIdle() const { return state_.load() == SubAgentState::Idle; }
    void setIdleRounds(int r) { idleRounds_ = r; }
    int idleRounds() const { return idleRounds_; }
    void incIdleRounds() { ++idleRounds_; }

private:
    std::string agentId_;
    std::string agentType_;
    std::string model_;
    std::string workspacePath_;
    std::shared_ptr<std::atomic<bool>> cancelFlag_;        // shared app-wide flag
    std::shared_ptr<std::atomic<bool>> agentCancelFlag_;    // per-Agent flag (individual cancel)
    std::atomic<SubAgentState> state_{SubAgentState::Idle};

    std::unique_ptr<DeepSeekClient> client_;
    ToolRegistry tools_;
    std::unique_ptr<Agent> agent_;
    SubAgentResult result_;
    mutable std::mutex resultMutex_;

    std::thread thread_;
    std::promise<SubAgentResult> promise_;
    mutable std::mutex statusMutex_;
    int idleRounds_ = 0;
};
