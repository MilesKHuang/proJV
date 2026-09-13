// proJV TUI -- status line implementation.
#include "status_line.h"

#include <string>

namespace status_line {

Line render(const AgentStatus& status) {
    switch (status.state) {
        case AgentState::Thinking: {
            bool hasReasoning = !status.reasoningText.empty();
            bool hasText = !status.streamingText.empty();
            if (hasReasoning && !hasText) {
                return {"Reasoning... (" + std::to_string(status.reasoningText.size()) + " chars)",
                        &ThemeColors::reasoningTextColor};
            }
            std::string s = "Generating... (" + std::to_string(status.streamingText.size()) + " chars)";
            if (hasReasoning) {
                s += " | reasoned " + std::to_string(status.reasoningText.size()) + " chars";
            }
            return {s, &ThemeColors::phaseStreaming};
        }
        case AgentState::ExecutingTool:
            return {"Running: " + status.currentToolName + " (" +
                        std::to_string(status.toolProgressCurrent) + "/" +
                        std::to_string(status.toolProgressTotal) + ")",
                    &ThemeColors::statusRunning};
        case AgentState::AwaitingApproval:
            return {"Awaiting approval...", &ThemeColors::statusAwaiting};
        case AgentState::Error:
            return {status.errorMessage, &ThemeColors::statusError};
        case AgentState::Idle:
        default:
            return {"Idle", &ThemeColors::statusIdle};
    }
}

} // namespace status_line
