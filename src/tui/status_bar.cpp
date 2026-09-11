// proJV TUI -- status bar implementation.
#include "status_bar.h"

#include <cstdio>
#include <string>

namespace status_bar {

namespace {

std::string fmtNum(int n) {
    char b[32];
    if (n >= 1000000) { snprintf(b, sizeof(b), "%.2fM", n / 1000000.0); return b; }
    if (n >= 1000)    { snprintf(b, sizeof(b), "%.2fK", n / 1000.0); return b; }
    snprintf(b, sizeof(b), "%d", n);
    return b;
}

const char* pressureName(Session::PressureLevel p) {
    switch (p) {
        case Session::PressureLevel::High:     return "HIGH";
        case Session::PressureLevel::Critical: return "CRITICAL";
        case Session::PressureLevel::Medium:   return "MEDIUM";
        case Session::PressureLevel::Low:
        default:                               return "LOW";
    }
}

} // namespace

std::string render(const Data& d) {
    std::string s;
    s += d.model;
    s += " tok: " + fmtNum(d.promptTokens) + "+" + fmtNum(d.completionTokens);
    s += " msgs: " + std::to_string(d.msgCount);
    if (d.toolCount > 0) s += " tools: " + std::to_string(d.toolCount);

    if (d.status.state == AgentState::Thinking) {
        s += std::string(" ctx: ") + pressureName(d.pressure);
        if (d.windowTokens > 0) {
            int pct = static_cast<int>(d.estimatedTokens * 100 / d.windowTokens);
            s += " (" + std::to_string(pct) + "%)";
        }
    }

    if (d.status.state == AgentState::ExecutingTool) {
        s += " running: " + d.status.currentToolName + " (" +
            std::to_string(d.status.toolProgressCurrent) + "/" +
            std::to_string(d.status.toolProgressTotal) + ")";
    } else if (d.status.state == AgentState::AwaitingApproval) {
        s += " awaiting approval";
    } else if (d.status.state == AgentState::Error) {
        s += " error: " + d.status.errorMessage;
    }

    s += " [ws: " + (d.workspace.empty() ? std::string("exe dir") : d.workspace) + "]";
    return s;
}

} // namespace status_bar
