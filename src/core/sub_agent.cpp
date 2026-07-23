#include "sub_agent.h"
#include "debug_log.h"
#include <chrono>

SubAgent::SubAgent(const std::string& agentId,
                   const std::string& agentType,
                   const std::string& model,
                   const std::string& workspacePath,
                   std::shared_ptr<std::atomic<bool>> cancelFlag)
    : agentId_(agentId)
    , agentType_(agentType)
    , model_(model)
    , workspacePath_(workspacePath)
    , cancelFlag_(cancelFlag)
{
    debugLogf("[SubAgent] Created %s type=%s model=%s", agentId_.c_str(), agentType_.c_str(), model_.c_str());
}

SubAgent::~SubAgent() {
    cancel();
    wait();
    debugLogf("[SubAgent] Destroyed %s", agentId_.c_str());
}

void SubAgent::cancel() {
    // Set per-Agent flag only -- does NOT touch the shared app-wide cancelFlag_.
    // This way, cancelling one SubAgent does not kill the Supervisor or other SubAgents.
    if (agentCancelFlag_) agentCancelFlag_->store(true, std::memory_order_release);
    if (client_) client_->cancel();
    state_.store(SubAgentState::Cancelled, std::memory_order_release);
}

SubAgentStatus SubAgent::getStatus() const {
    std::lock_guard<std::mutex> lk(statusMutex_);
    SubAgentStatus s;
    s.agentId = agentId_;
    s.agentType = agentType_;
    s.state = state_.load();
    s.elapsedMs = 0;
    s.idleRounds = idleRounds_;

    if (agent_) {
        auto ast = agent_->getStatus();
        s.phase = agent_->getPhase();
        s.currentTool = ast.currentToolName;
        s.toolProgressCurrent = ast.toolProgressCurrent;
        s.toolProgressTotal = ast.toolProgressTotal;
        // Get the last assistant text (even if followed by tool_calls) for progress visibility
        auto msgs = agent_->getSession().getContextMessages();
        for (auto it = msgs.rbegin(); it != msgs.rend(); ++it) {
            if (it->role == "assistant" && !it->content.empty()) {
                s.lastReply = it->content;
                break;
            }
        }
    }
    return s;
}

SubAgentResult SubAgent::getResult() const {
    std::lock_guard<std::mutex> lk(resultMutex_);
    return result_;
}

void SubAgent::wait() {
    if (thread_.joinable()) {
        try { thread_.join(); } catch (...) {}
    }
}
