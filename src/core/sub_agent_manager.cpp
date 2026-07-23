#include "sub_agent_manager.h"
#include "tool_list_parser.h"
#include "tools/registry.h"
#include "tools/shell_tool.h"
#include "tools/file_tool.h"
#include "tools/md_file_tool.h"
#include "tools/web_tools.h"
#include "debug_log.h"
#include <algorithm>
#include <chrono>
#include <thread>

SubAgentManager::SubAgentManager(std::shared_ptr<std::atomic<bool>> cancelFlag,
                                 AgentRegistry& registry,
                                 const std::string& workspacePath)
    : cancelFlag_(cancelFlag)
    , registry_(registry)
    , workspacePath_(workspacePath)
{
}

SubAgentManager::~SubAgentManager() {
    cancelAll();
}

// Helper: register tools for SubAgent by parsing allowed tools from prompt
static void buildSubAgentTools(ToolRegistry& tools, const std::string& promptContent,
                                const std::string& workspacePath) {
    auto allowedNames = parseAvailableTools(promptContent);
    for (auto& name : allowedNames) {
        registerToolByName(tools, name, workspacePath);
    }
}

std::string SubAgentManager::spawn(const std::string& agentType,
                                    const std::string& task,
                                    const std::string& taskContext) {
    try {
    std::lock_guard<std::mutex> lk(mutex_);

    // Count Running agents directly (we already hold mutex_ -- can't call activeCount()
    // which would lock the same non-recursive mutex and throw in MSVC Debug mode).
    int running = 0;
    for (auto& [id, sa] : agents_)
        if (sa->state() == SubAgentState::Running) ++running;
    if (running >= MAX_CONCURRENT_SUBAGENTS) {
        return "";
    }
    auto cfg = registry_.findType(agentType);
    if (!cfg) {
        debugLogf("[SubAgentManager] Unknown agent type: %s", agentType.c_str());
        return "";
    }

    debugLogf("[SubAgentManager] spawn step2: new SubAgent");
    std::string agentId = agentType + "_" + std::to_string(nextId_++);
    auto sa = std::make_unique<SubAgent>(agentId, agentType, cfg->model,
                                          workspacePath_, cancelFlag_);

    debugLogf("[SubAgentManager] spawn step3: buildTools");
    buildSubAgentTools(sa->tools_, cfg->promptContent, workspacePath_);

    debugLogf("[SubAgentManager] spawn step4: new DeepSeekClient");
    sa->client_ = std::make_unique<DeepSeekClient>();
    if (!cfg_.apiKey.empty()) {
        sa->client_->setConfig(cfg_);
    }

    debugLogf("[SubAgentManager] spawn step5: agentFlag + new Agent");
    auto agentFlag = std::make_shared<std::atomic<bool>>(false);
    sa->agentCancelFlag_ = agentFlag;
    sa->agent_ = std::make_unique<Agent>(*sa->client_, sa->tools_, agentFlag, true);
    sa->agent_->setModel(cfg->model.empty() ? "deepseek-v4-flash" : cfg->model);
    sa->agent_->setWorkspacePath(workspacePath_);
    sa->agent_->setRequestParams(8192, 0.0);

    debugLogf("[SubAgentManager] spawn step6: inject prompts");
    sa->agent_->addPersistedMessage(Message::System(cfg->promptContent));
    std::string ctxMsg = "You are working on a sub-task delegated by the Supervisor.\n";
    ctxMsg += "Task: " + task + "\n";
    if (!taskContext.empty()) {
        ctxMsg += "Context: " + taskContext + "\n";
    }
    ctxMsg += "Report back when done. Do NOT ask the user questions -- the Supervisor will handle that.\n";
    ctxMsg += "Keep responses concise -- compact conclusions, not essay paragraphs.";
    sa->agent_->addPersistedMessage(Message::System(ctxMsg));

    debugLogf("[SubAgentManager] spawn: creating thread for %s", agentId.c_str());
    // Launch thread
    sa->state_.store(SubAgentState::Running, std::memory_order_release);
    auto startTime = std::chrono::steady_clock::now();

    sa->promise_ = std::promise<SubAgentResult>();
    auto future = sa->promise_.get_future();

    try {
        sa->thread_ = std::thread([this, saPtr = sa.get(), task, startTime]() {
            try {
                saPtr->agent_->startTurn(task);
                saPtr->agent_->run();

            auto msgs = saPtr->agent_->getSession().getContextMessages();
            {
                std::lock_guard<std::mutex> rlk(saPtr->resultMutex_);
                saPtr->result_.agentId = saPtr->agentId_;
                saPtr->result_.success = true;

                for (auto it = msgs.rbegin(); it != msgs.rend(); ++it) {
                    if (it->role == "assistant" && !it->content.empty() && it->toolCalls.empty()) {
                        saPtr->result_.finalReply = it->content;
                        break;
                    }
                }
                for (auto& m : msgs) {
                    if (m.role == "assistant" && !m.toolCalls.empty())
                        saPtr->result_.toolCalls += (int)m.toolCalls.size();
                }
                auto endTime = std::chrono::steady_clock::now();
                saPtr->result_.elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    endTime - startTime).count();

                // Check both shared app-wide flag AND per-Agent flag
                if (saPtr->cancelFlag_->load() || saPtr->agentCancelFlag_->load()) {
                    saPtr->state_.store(SubAgentState::Cancelled, std::memory_order_release);
                    saPtr->result_.success = false;
                    saPtr->result_.errorMessage = "Cancelled";
                } else {
                    saPtr->state_.store(SubAgentState::Completed, std::memory_order_release);
                    hasCompletedFlag_.store(true, std::memory_order_release);
                }
            }
            try { saPtr->promise_.set_value(saPtr->result_); } catch (...) {}
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> rlk(saPtr->resultMutex_);
            saPtr->result_.agentId = saPtr->agentId_;
            saPtr->result_.success = false;
            saPtr->result_.errorMessage = e.what();
            auto endTime = std::chrono::steady_clock::now();
            saPtr->result_.elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                endTime - startTime).count();
            saPtr->state_.store(SubAgentState::Failed, std::memory_order_release);
            try { saPtr->promise_.set_value(saPtr->result_); } catch (...) {}
        }
    });
    } catch (const std::exception& e) {
        // Thread creation failed (e.g. "device or resource busy")
        sa->state_.store(SubAgentState::Failed, std::memory_order_release);
        debugLogf("[SubAgentManager] Thread creation failed for %s: %s", agentId.c_str(), e.what());
        return ""; // empty agentId signals failure to caller
    }

    auto id = agentId;
    agents_[id] = std::move(sa);
    debugLogf("[SubAgentManager] Spawned %s type=%s task=%s", id.c_str(), agentType.c_str(),
              task.substr(0, 80).c_str());
    return id;
    } catch (const std::exception& e) {
        debugLogf("[SubAgentManager] spawn FAILED: %s", e.what());
        return "";
    }
}
std::future<SubAgentResult> SubAgentManager::reuse(const std::string& agentId,
                                                    const std::string& task,
                                                    const std::string& taskContext) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = agents_.find(agentId);
    if (it == agents_.end()) {
        std::promise<SubAgentResult> p;
        SubAgentResult r; r.agentId = agentId; r.success = false;
        r.errorMessage = "Agent not found";
        p.set_value(r);
        return p.get_future();
    }

    auto& sa = *it->second;
    if (!sa.isDone() && !sa.isIdle()) {
        std::promise<SubAgentResult> p;
        SubAgentResult r; r.agentId = agentId; r.success = false;
        r.errorMessage = "Agent is still running";
        p.set_value(r);
        return p.get_future();
    }

    sa.wait();
    sa.state_.store(SubAgentState::Running, std::memory_order_release);

    auto startTime = std::chrono::steady_clock::now();
    sa.promise_ = std::promise<SubAgentResult>();
    auto future = sa.promise_.get_future();

    std::string ctxMsg = "New sub-task delegated by the Supervisor:\n";
    ctxMsg += "Task: " + task + "\n";
    if (!taskContext.empty()) ctxMsg += "Context: " + taskContext + "\n";
    ctxMsg += "Report back when done.";
    sa.agent_->addPersistedMessage(Message::System(ctxMsg));

    try {
        sa.thread_ = std::thread([this, saPtr = &sa, task, startTime]() {
            try {
                saPtr->agent_->startTurn(task);
                saPtr->agent_->run();
            auto msgs = saPtr->agent_->getSession().getContextMessages();
            {
                std::lock_guard<std::mutex> rlk(saPtr->resultMutex_);
                saPtr->result_.agentId = saPtr->agentId_;
                saPtr->result_.success = true;
                for (auto it = msgs.rbegin(); it != msgs.rend(); ++it) {
                    if (it->role == "assistant" && !it->content.empty() && it->toolCalls.empty()) {
                        saPtr->result_.finalReply = it->content; break;
                    }
                }
                saPtr->result_.toolCalls = 0;
                for (auto& m : msgs) {
                    if (m.role == "assistant" && !m.toolCalls.empty())
                        saPtr->result_.toolCalls += (int)m.toolCalls.size();
                }
                auto endTime = std::chrono::steady_clock::now();
                saPtr->result_.elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    endTime - startTime).count();
                if (saPtr->cancelFlag_->load() || saPtr->agentCancelFlag_->load()) {
                    saPtr->state_.store(SubAgentState::Cancelled, std::memory_order_release);
                    saPtr->result_.success = false;
                } else {
                    saPtr->state_.store(SubAgentState::Completed, std::memory_order_release);
                    hasCompletedFlag_.store(true, std::memory_order_release);
                }
            }
            try { saPtr->promise_.set_value(saPtr->result_); } catch (...) {}
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> rlk(saPtr->resultMutex_);
            saPtr->result_.agentId = saPtr->agentId_;
            saPtr->result_.success = false;
            saPtr->result_.errorMessage = e.what();
            saPtr->state_.store(SubAgentState::Failed, std::memory_order_release);
            try { saPtr->promise_.set_value(saPtr->result_); } catch (...) {}
        }
    });
    } catch (const std::exception& e) {
        sa.state_.store(SubAgentState::Failed, std::memory_order_release);
        debugLogf("[SubAgentManager] Thread reuse failed for %s: %s", agentId.c_str(), e.what());
        SubAgentResult r; r.agentId = agentId; r.success = false;
        r.errorMessage = std::string("Thread error: ") + e.what();
        std::promise<SubAgentResult> p;
        p.set_value(r);
        return p.get_future();
    }
    debugLogf("[SubAgentManager] Reusing %s", agentId.c_str());
    return future;
}

void SubAgentManager::cancel(const std::string& agentId) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = agents_.find(agentId);
    if (it != agents_.end()) {
        it->second->cancel();
        it->second->wait();
        agents_.erase(it);  // auto-reap: cancelled agents don't linger
        debugLogf("[SubAgentManager] Cancelled and reaped %s", agentId.c_str());
    }
}

void SubAgentManager::cancelAll() {
    std::lock_guard<std::mutex> lk(mutex_);
    cancelFlag_->store(true, std::memory_order_release);
    for (auto& [id, sa] : agents_) { sa->cancel(); sa->wait(); }
    agents_.clear();
    debugLog("[SubAgentManager] All sub-agents cancelled");
}

void SubAgentManager::reap(const std::string& agentId) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = agents_.find(agentId);
    if (it != agents_.end()) {
        if (it->second->state() == SubAgentState::Running) it->second->cancel();
        it->second->wait();
        agents_.erase(it);
        debugLogf("[SubAgentManager] Reaped %s", agentId.c_str());
    }
}

void SubAgentManager::reapCompleted() {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<std::string> toReap;
    for (auto& [id, sa] : agents_) if (sa->isDone()) toReap.push_back(id);
    for (auto& id : toReap) { agents_.erase(id); debugLogf("[SubAgentManager] Auto-reaped %s", id.c_str()); }
}

SubAgentStatus SubAgentManager::getStatus(const std::string& agentId) const {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = agents_.find(agentId);
    if (it != agents_.end()) return it->second->getStatus();
    SubAgentStatus s; s.agentId = agentId; s.state = SubAgentState::Cancelled;
    return s;
}

std::vector<SubAgentStatus> SubAgentManager::getAllStatuses() const {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<SubAgentStatus> result;
    for (auto& [id, sa] : agents_) result.push_back(sa->getStatus());
    return result;
}

std::vector<SubAgentResult> SubAgentManager::drainCompletedResults() {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<SubAgentResult> results;
    std::vector<std::string> toReap;
    for (auto& [id, sa] : agents_) {
        if (sa->isDone() || sa->state() == SubAgentState::Completed) {
            auto r = sa->getResult();
            if (!r.finalReply.empty()) {
                results.push_back(r);
            }
            toReap.push_back(id);
        }
    }
    for (auto& id : toReap) {
        agents_.erase(id);
    }
    if (!results.empty()) {
        debugLogf("[SubAgentManager] Drained %zu completed sub-agents", results.size());
    }
    hasCompletedFlag_.store(false, std::memory_order_release);
    return results;
}

int SubAgentManager::activeCount() const {
    std::lock_guard<std::mutex> lk(mutex_);
    int count = 0;
    for (auto& [id, sa] : agents_)
        if (sa->state() == SubAgentState::Running) ++count;
    return count;
}

std::vector<std::string> SubAgentManager::getAgentIds() const {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<std::string> ids;
    for (auto& [id, sa] : agents_) ids.push_back(id);
    return ids;
}
