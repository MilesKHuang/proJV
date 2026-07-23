#pragma once
#include "core/agent.h"
#include "core/sub_agent_manager.h"
#include <memory>

// Supervisor: extends Agent with sub-agent delegation capability.
// This is a thin wrapper that holds a reference to SubAgentManager.
class Supervisor {
public:
    Supervisor(Agent& agent, SubAgentManager& subAgentMgr);

    Agent& getAgent() { return agent_; }
    SubAgentManager& getSubAgentManager() { return subAgentMgr_; }

    // Convenience: cancel both the agent and all sub-agents
    void cancelAll();

private:
    Agent& agent_;
    SubAgentManager& subAgentMgr_;
};
