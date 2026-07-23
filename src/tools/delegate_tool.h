#pragma once
#include "tools/registry.h"
#include "core/sub_agent_manager.h"

// Register the delegate_task tool on the given ToolRegistry.
// The SubAgentManager reference is captured by the executor lambda.
void registerDelegateTaskTool(ToolRegistry& registry, SubAgentManager& mgr);
