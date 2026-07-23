#include "supervisor.h"

Supervisor::Supervisor(Agent& agent, SubAgentManager& subAgentMgr)
    : agent_(agent), subAgentMgr_(subAgentMgr)
{
}

void Supervisor::cancelAll() {
    subAgentMgr_.cancelAll();
    agent_.cancel();
}
