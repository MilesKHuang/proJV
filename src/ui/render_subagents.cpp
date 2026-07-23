#include "render_subagents.h"
#include "ui/theme.h"
#include <imgui.h>
#include <format>
#include <algorithm>

static constexpr float kInlineHeight = 200.0f;

void renderSubAgentPanelInline(SubAgentManager* mgr) {
    if (!mgr) return;
    const auto& T = ThemeManager::instance().current();

    auto statuses = mgr->getAllStatuses();
    int total = (int)statuses.size();
    int max = SubAgentManager::MAX_CONCURRENT_SUBAGENTS;

    ImGui::BeginChild("SubAgentInline", ImVec2(0, kInlineHeight), true,
        ImGuiWindowFlags_AlwaysVerticalScrollbar);

    ImGui::TextColored(ThemeColors::toVec4(T.todoTitle), "SUB-AGENTS [%d/%d]", total, max);
    ImGui::Separator();

    // Only show Running agents; completed/cancelled are invisible
    int visibleCount = 0;
    for (auto& s : statuses) {
        if (s.state != SubAgentState::Running) continue;
        ++visibleCount;
    }
    if (visibleCount == 0) {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "  (No active sub-agents)");
        ImGui::EndChild();
        return;
    }

    for (auto& s : statuses) {
        if (s.state != SubAgentState::Running) continue;
        std::string marker;
        ImVec4 color;
        switch (s.state) {
            case SubAgentState::Running:
                marker = "[>]";
                color = ImVec4(0.0f, 1.0f, 0.0f, 1.0f);
                break;
            case SubAgentState::Completed:
                marker = "[OK]";
                color = ImVec4(0.0f, 0.8f, 0.0f, 1.0f);
                break;
            case SubAgentState::Failed:
                marker = "[XX]";
                color = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
                break;
            case SubAgentState::Cancelled:
                marker = "[!!]";
                color = ImVec4(1.0f, 0.8f, 0.0f, 1.0f);
                break;
            case SubAgentState::Idle:
                marker = "[--]";
                color = ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
                break;
        }

        std::string line = marker + " " + s.agentType;
        ImGui::TextColored(color, "%s", line.c_str());

        // Elapsed time and phase
        ImGui::SameLine();
        std::string stateStr;
        switch (s.state) {
            case SubAgentState::Running: stateStr = "Running"; break;
            case SubAgentState::Completed: stateStr = "Done"; break;
            case SubAgentState::Failed: stateStr = "Failed"; break;
            case SubAgentState::Cancelled: stateStr = "Cancelled"; break;
            case SubAgentState::Idle: stateStr = "Idle"; break;
        }
        if (s.elapsedMs > 0) {
            int secs = (int)(s.elapsedMs / 1000);
            if (secs >= 60)
                stateStr += std::format(" {}m", secs / 60);
            else
                stateStr += std::format(" {}s", secs);
        }
        ImGui::SameLine(0, 6);
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s", stateStr.c_str());

        if (s.state == SubAgentState::Running && !s.currentTool.empty()) {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "    %s (%d/%d)",
                s.currentTool.c_str(), s.toolProgressCurrent, s.toolProgressTotal);
        }
    }

    ImGui::EndChild();
}


