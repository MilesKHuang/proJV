#include "delegate_tool.h"
#include "json_utils.h"
#include "json.hpp"
#include "debug_log.h"

static constexpr const char* TOOL_DELEGATE_DESC =
    "Delegate a task to a specialized sub-agent for parallel or isolated execution.\n"
    "Use this tool to spawn, monitor, cancel, list, or reap sub-agents.\n\n"
    "ACTIONS:\n"
    "  start  -- Create and run a new sub-agent with the given task.\n"
    "  status -- Poll progress of a running sub-agent.\n"
    "  cancel -- Cancel a sub-agent.\n"
    "  list   -- List all existing sub-agents and their states.\n"
    "  reap   -- Destroy a completed/cancelled/failed sub-agent to free resources.\n\n"
    "PARAMETERS for action=start:\n"
    "  agent_type: Sub-agent type name (coder/designer/analyzer/tester). Required.\n"
    "  task: Self-contained task description including goal, constraints, expected output format, and relevant file paths. Required.\n"
    "  context: Additional context (optional).\n"
    "  reuse_id: Existing agent ID to reuse instead of creating new (optional).\n\n"
    "PARAMETERS for action=status/cancel/reap:\n"
    "  agent_id: The sub-agent ID. Required.";

void registerDelegateTaskTool(ToolRegistry& registry, SubAgentManager& mgr) {
    ToolDefinition def;
    def.name = "delegate_task";
    def.description = TOOL_DELEGATE_DESC;
    def.parameters = {
        {"action", "string", "Action: start, status, cancel, list, or reap", true},
        {"agent_type", "string", "Sub-agent type name (action=start, required)", false},
        {"task", "string", "Task description (action=start, required)", false},
        {"context", "string", "Additional context (action=start, optional)", false},
        {"agent_id", "string", "Sub-agent ID (action=status/cancel/reap, required)", false},
        {"reuse_id", "string", "Existing agent ID to reuse (action=start, optional)", false}
    };

    registry.registerTool(def, [&mgr](const std::string& args) -> std::string {
        std::string action = extractStringArg(args, "action");

        // --- action: list ---
        if (action == "list") {
            auto statuses = mgr.getAllStatuses();
            if (statuses.empty()) {
                return "{\"agents\": []}";
            }
            nlohmann::json j;
            j["agents"] = nlohmann::json::array();
            for (auto& s : statuses) {
                nlohmann::json item;
                item["agent_id"] = s.agentId;
                item["type"] = s.agentType;
                std::string stateStr;
                switch (s.state) {
                    case SubAgentState::Idle: stateStr = "idle"; break;
                    case SubAgentState::Running: stateStr = "running"; break;
                    case SubAgentState::Completed: stateStr = "completed"; break;
                    case SubAgentState::Failed: stateStr = "failed"; break;
                    case SubAgentState::Cancelled: stateStr = "cancelled"; break;
                }
                item["status"] = stateStr;
                item["task_summary"] = s.taskSummary;
                item["idle"] = (s.state == SubAgentState::Idle || s.state == SubAgentState::Completed);
                j["agents"].push_back(item);
            }
            return j.dump();
        }

        // --- action: start ---
        if (action == "start") {
            std::string reuseId = extractStringArg(args, "reuse_id");
            std::string agentType = extractStringArg(args, "agent_type");
            std::string task = extractStringArg(args, "task");
            std::string context = extractStringArg(args, "context");

            if (agentType.empty() && reuseId.empty()) {
                return "Error: delegate_task action=start requires 'agent_type' or 'reuse_id'";
            }
            if (task.empty()) {
                return "Error: delegate_task action=start requires 'task'";
            }

            // Check if reusing
            if (!reuseId.empty()) {
                // Reuse async — returns immediately, results in next turn
                mgr.reuse(reuseId, task, context);
                nlohmann::json j;
                j["agent_id"] = reuseId;
                j["status"] = "running";
                j["message"] = "Sub-agent reused. Results in next turn.";
                return j.dump();
            }

            // Spawn new SubAgent asynchronously — returns immediately.
            // Results will be injected at the start of the next turn.
            std::string agentId = mgr.spawn(agentType, task, context);
            if (agentId.empty()) {
                return "Error: Cannot spawn sub-agent. Possible causes: "
                       "max concurrent limit (5) reached, unknown agent type, "
                       "or system resource limit (thread creation failed). "
                       "Use action=list to check existing agents.";
            }

            nlohmann::json j;
            j["agent_id"] = agentId;
            j["status"] = "running";
            j["message"] = "Sub-agent started. Results will appear automatically at the start of your next turn.";
            debugLogf("[DelegateTool] Spawned %s for task: %s", agentId.c_str(), task.substr(0, 100).c_str());
            return j.dump();
        }

        // --- action: status ---
        if (action == "status") {
            std::string agentId = extractStringArg(args, "agent_id");
            if (agentId.empty()) return "Error: action=status requires 'agent_id'";

            auto s = mgr.getStatus(agentId);
            nlohmann::json j;
            j["agent_id"] = s.agentId;
            std::string stateStr;
            switch (s.state) {
                case SubAgentState::Idle: stateStr = "idle"; break;
                case SubAgentState::Running: stateStr = "running"; break;
                case SubAgentState::Completed: stateStr = "completed"; break;
                case SubAgentState::Failed: stateStr = "failed"; break;
                case SubAgentState::Cancelled: stateStr = "cancelled"; break;
            }
            j["status"] = stateStr;
            std::string phaseStr;
            switch (s.phase) {
                case AgentPhase::Idle: phaseStr = "idle"; break;
                case AgentPhase::Streaming: phaseStr = "streaming"; break;
                case AgentPhase::ExecutingTools: phaseStr = "executing_tools"; break;
                case AgentPhase::AwaitApproval: phaseStr = "awaiting_approval"; break;
                case AgentPhase::Error: phaseStr = "error"; break;
            }
            j["phase"] = phaseStr;
            j["current_tool"] = s.currentTool;
            j["progress"] = std::to_string(s.toolProgressCurrent) + "/" + std::to_string(s.toolProgressTotal);
            j["elapsed_ms"] = s.elapsedMs;
            if (!s.lastReply.empty()) {
                // Truncate to 300 chars to stay concise
                std::string lr = s.lastReply.size() > 300
                    ? s.lastReply.substr(0, 300) + "..."
                    : s.lastReply;
                j["last_reply"] = lr;
            }
            return j.dump();
        }

        // --- action: cancel ---
        if (action == "cancel") {
            std::string agentId = extractStringArg(args, "agent_id");
            if (agentId.empty()) return "Error: action=cancel requires 'agent_id'";
            mgr.cancel(agentId);
            nlohmann::json j;
            j["agent_id"] = agentId;
            j["status"] = "cancelled";
            return j.dump();
        }

        // --- action: reap ---
        if (action == "reap") {
            std::string agentId = extractStringArg(args, "agent_id");
            if (agentId.empty()) return "Error: action=reap requires 'agent_id'";
            mgr.reap(agentId);
            nlohmann::json j;
            j["agent_id"] = agentId;
            j["status"] = "reaped";
            return j.dump();
        }

        return "Error: delegate_task requires action='start'|'status'|'cancel'|'list'|'reap'. "
               "You passed action='" + action + "'.";
    });
}
