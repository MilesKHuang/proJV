#pragma once
#include "sub_agent.h"
#include "agent_registry.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <future>
#include <vector>

// Manages all SubAgent instances. Lifecycle owned by App, spans across conversation turns.
class SubAgentManager {
public:
    static constexpr int MAX_CONCURRENT_SUBAGENTS = 5;

    SubAgentManager(std::shared_ptr<std::atomic<bool>> cancelFlag,
                    AgentRegistry& registry,
                    const std::string& workspacePath);
    ~SubAgentManager();

    // ---- Lifecycle ----

    // Spawn a new SubAgent. Returns agent_id if successful, empty if at capacity.
    std::string spawn(const std::string& agentType,
                      const std::string& task,
                      const std::string& taskContext);

    // Reuse an existing SubAgent with new task. Returns future of result.
    // Only works if SubAgent is Idle or Completed.
    std::future<SubAgentResult> reuse(const std::string& agentId,
                                       const std::string& task,
                                       const std::string& taskContext);

    // Cancel a specific SubAgent.
    void cancel(const std::string& agentId);

    // Cancel all SubAgents.
    void cancelAll();

    // Reap (destroy) a specific SubAgent.
    void reap(const std::string& agentId);

    // Reap all completed/failed/cancelled SubAgents.
    void reapCompleted();

    // ---- Status ----

    SubAgentStatus getStatus(const std::string& agentId) const;
    std::vector<SubAgentStatus> getAllStatuses() const;

    // Drain all completed SubAgents: return their results and reap them.
    // Called at the start of each Supervisor turn to inject results.
    std::vector<SubAgentResult> drainCompletedResults();

    // Check if any SubAgent has completed since last drain. Atomic, lock-free.
    bool hasCompleted() const { return hasCompletedFlag_.load(std::memory_order_acquire); }

    // ---- Queries ----

    int activeCount() const;
    std::vector<std::string> getAgentIds() const;
    const std::string& workspacePath() const { return workspacePath_; }

    // Set config for new SubAgent clients (API key, base URL, etc.)
    void setClientConfig(const AppConfig& cfg) { cfg_ = cfg; }

private:
    std::shared_ptr<std::atomic<bool>> cancelFlag_;
    AgentRegistry& registry_;
    std::string workspacePath_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<SubAgent>> agents_;
    std::atomic<bool> hasCompletedFlag_{false};  // set by SubAgent threads on completion
    AppConfig cfg_;  // config for new SubAgent clients
    int nextId_ = 1;

};
