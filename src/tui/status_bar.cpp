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

} // namespace

std::vector<Segment> renderSegments(const Data& d) {
    std::vector<Segment> segs;

    segs.push_back({d.model, &ThemeColors::statusModelName});
    segs.push_back({" tok: " + fmtNum(d.promptTokens) + "+" + fmtNum(d.completionTokens),
                    &ThemeColors::statusTokenCount});
    segs.push_back({" msgs: " + std::to_string(d.msgCount), &ThemeColors::statusMsgCount});

    if (d.toolCount > 0) {
        segs.push_back({" tools: " + std::to_string(d.toolCount), &ThemeColors::statusToolCount});
    }

    if (d.status.state == AgentState::Thinking) {
        std::string name = "LOW";
        std::string ThemeColors::* ctxColor = &ThemeColors::statusCtxLow;
        switch (d.pressure) {
            case Session::PressureLevel::High:
                name = "HIGH"; ctxColor = &ThemeColors::statusCtxHigh; break;
            case Session::PressureLevel::Critical:
                name = "CRITICAL"; ctxColor = &ThemeColors::statusCtxCritical; break;
            case Session::PressureLevel::Medium:
                name = "MEDIUM"; ctxColor = &ThemeColors::statusCtxMedium; break;
            case Session::PressureLevel::Low:
            default:
                name = "LOW"; ctxColor = &ThemeColors::statusCtxLow; break;
        }
        segs.push_back({" ctx: " + name, ctxColor});
        if (d.windowTokens > 0) {
            int pct = static_cast<int>(d.estimatedTokens * 100 / d.windowTokens);
            segs.push_back({" (" + std::to_string(pct) + "%)", &ThemeColors::statusCtxPercent});
        }
    }

    if (d.status.state == AgentState::ExecutingTool) {
        segs.push_back({" running: " + d.status.currentToolName + " (" +
            std::to_string(d.status.toolProgressCurrent) + "/" +
            std::to_string(d.status.toolProgressTotal) + ")",
            &ThemeColors::statusRunning});
    } else if (d.status.state == AgentState::AwaitingApproval) {
        segs.push_back({" awaiting approval", &ThemeColors::statusAwaiting});
    } else if (d.status.state == AgentState::Error) {
        segs.push_back({" error: " + d.status.errorMessage, &ThemeColors::statusError});
    }

    segs.push_back({" [ws: " + (d.workspace.empty() ? std::string("exe dir") : d.workspace) + "]",
                    &ThemeColors::statusWorkspace});

    if (d.cost >= 0.005) {
        char b[32];
        snprintf(b, sizeof(b), " ~$%.2f", d.cost);
        segs.push_back({b, &ThemeColors::statusTokenInfo});
    }

    return segs;
}

std::string render(const Data& d) {
    std::string s;
    for (const auto& seg : renderSegments(d)) s += seg.text;
    return s;
}

} // namespace status_bar
