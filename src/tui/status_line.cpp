// proJV TUI -- status line implementation.
#include "status_line.h"

#include <string>

namespace status_line {

std::string render(const AgentStatus& status) {
    switch (status.state) {
        case AgentState::Thinking: {
            bool hasReasoning = !status.reasoningText.empty();
            bool hasText = !status.streamingText.empty();
            if (hasReasoning && !hasText) {
                return "Reasoning... (" + std::to_string(status.reasoningText.size()) + " chars)";
            }
            std::string s = "Generating... (" + std::to_string(status.streamingText.size()) + " chars)";
            if (hasReasoning) {
                s += " | reasoned " + std::to_string(status.reasoningText.size()) + " chars";
            }
            return s;
        }
        case AgentState::ExecutingTool:
            return "Running: " + status.currentToolName + " (" +
                std::to_string(status.toolProgressCurrent) + "/" +
                std::to_string(status.toolProgressTotal) + ")";
        case AgentState::AwaitingApproval:
            return "Awaiting approval...";
        case AgentState::Error:
            return status.errorMessage;
        case AgentState::Idle:
        default:
            return "Idle";
    }
}

} // namespace status_line
