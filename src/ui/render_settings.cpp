// --- Settings dialogs: config, system prompt, tool approval ----------
// Extracted from app.cpp for maintainability.

#include "app.h"
#include "core/config.h"
#include "json.hpp"
#include "debug_log.h"
#include <windows.h>
#include <imgui.h>
#include <commdlg.h>
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;

// --- App::renderToolApprovalDialog --------------------------------
void App::renderToolApprovalDialog() {
    if (!agent) return;
    if (!ImGui::BeginPopupModal("Delete Confirmation", nullptr,
        ImGuiWindowFlags_AlwaysAutoResize))
        return;

    auto status = agent->getStatus();
    if (status.state != AgentState::AwaitingApproval) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    auto msgs = agent->getSession().getContextMessages();
    if (msgs.empty()) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
    const auto& toolCalls = msgs.back().toolCalls;

    ImGui::TextUnformatted(" DELETE CONFIRMATION");
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextWrapped("The AI wants to DELETE files. Review the operation below:");
    ImGui::Spacing();

    ImGui::BeginChild("DelList", ImVec2(520, 0),
        ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);

    for (const auto& call : toolCalls) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
        ImGui::TextUnformatted(" Command:");
        ImGui::PopStyleColor();

        std::string cmd;
        try { cmd = nlohmann::json::parse(call.arguments).value("command", ""); }
        catch (...) { cmd = call.arguments; }

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.35f, 1.0f));
        ImGui::TextWrapped("%s", cmd.c_str());
        ImGui::PopStyleColor();

        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.0f, 1.0f));
        ImGui::TextUnformatted(" Files to delete:");
        ImGui::PopStyleColor();

        std::vector<std::string> files;
        std::string clower = cmd;
        for (auto& c : clower) c = (char)tolower((unsigned char)c);
        size_t cmdPos = std::string::npos;
        const char* deleteKws[] = {"del ", "erase ", "rm ", "rmdir ", "rd "};
        for (const auto& kw : deleteKws) {
            size_t pos = clower.find(kw);
            while (pos != std::string::npos) {
                bool ok = (pos == 0) || (clower[pos - 1] == ' ' || clower[pos - 1] == '\t');
                if (ok) {
                    cmdPos = pos + strlen(kw);
                    break;
                }
                pos = clower.find(kw, pos + 1);
            }
            if (cmdPos != std::string::npos) break;
        }
        if (cmdPos != std::string::npos) {
            std::string args = cmd.substr(cmdPos);
            std::string current;
            for (size_t i = 0; i < args.size(); ++i) {
                char c = args[i];
                if (c == ' ') {
                    if (!current.empty()) {
                        if (current[0] != '/' && current[0] != '-')
                            files.push_back(current);
                        current.clear();
                    }
                } else if (c == '"') {
                    ++i;
                    while (i < args.size() && args[i] != '"')
                        current += args[i++];
                    if (!current.empty()) { files.push_back(current); current.clear(); }
                } else {
                    current += c;
                }
            }
            if (!current.empty() && current[0] != '/' && current[0] != '-')
                files.push_back(current);
        }

        if (files.empty())
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "  %s", call.arguments.c_str());
        else
            for (const auto& f : files) ImGui::BulletText("%s", f.c_str());
        ImGui::Separator();
    }

    ImGui::EndChild();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("Approve this delete operation?");
    ImGui::Spacing();

    if (ImGui::Button("YES \u2014 Execute Once", ImVec2(200, 32))) {
        agent->approveTool(1);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("ALWAYS \u2014 Allow This Session", ImVec2(240, 32))) {
        agent->approveTool(2);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("NO \u2014 Cancel", ImVec2(160, 32))) {
        agent->approveTool(0);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// --- App::renderConfigPopup ----------------------------------------
void App::renderConfigPopup() {
    bool open = true;
    if (!ImGui::BeginPopupModal("Configuration", &open, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    ImGui::Text("DeepSeek API Configuration");
    ImGui::Separator();
    ImGui::Spacing();

    // -- API Key ----------------------------------------------------
    ImGui::Text("API Key:");
    ImGui::SameLine();
    ImGui::InputText("##cfg_apikey", apiKeyBuf, sizeof(apiKeyBuf),
        ImGuiInputTextFlags_Password);
    ImGui::Spacing();

    // -- Base URL ---------------------------------------------------
    ImGui::Text("Base URL:");
    ImGui::SameLine();
    ImGui::InputText("##cfg_baseurl", baseUrlBuf, sizeof(baseUrlBuf));
    ImGui::Spacing();

    // -- Max Tokens -------------------------------------------------
    ImGui::Text("Max Tokens:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180.0f);
    ImGui::SliderInt("##cfg_maxtokens", &configMaxTokens, 512, 16384, "%d");
    ImGui::Spacing();

    // -- Temperature ------------------------------------------------
    ImGui::Text("Temperature:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180.0f);
    ImGui::SliderFloat("##cfg_temp", &configTemperature, 0.0f, 2.0f, "%.2f");
    ImGui::Spacing();

    // -- Workspace Path ---------------------------------------------
    ImGui::Text("Workspace:");
    ImGui::SameLine();
    ImGui::InputText("##cfg_ws", workspacePathBuf, sizeof(workspacePathBuf));
    ImGui::Spacing();

    // -- C++ Compiler Path ------------------------------------------
    ImGui::Text("C++ Compiler:");
    ImGui::SameLine();
    ImGui::InputText("##cfg_cpp", cppCompilerPathBuf, sizeof(cppCompilerPathBuf));
    ImGui::Spacing();

    // -- Python Path ------------------------------------------------
    ImGui::Text("Python:");
    ImGui::SameLine();
    ImGui::InputText("##cfg_python", pythonPathBuf, sizeof(pythonPathBuf));
    ImGui::Spacing();

    ImGui::Separator();
    ImGui::Spacing();

    // -- Buttons ----------------------------------------------------
    if (ImGui::Button("Save && Connect", ImVec2(180, 32))) {
        // Write all fields back to config
        config.apiKey = apiKeyBuf;
        config.baseUrl = baseUrlBuf;
        config.maxTokens = configMaxTokens;
        config.temperature = static_cast<double>(configTemperature);
        config.workspacePath = workspacePathBuf;
        config.cppCompilerPath = cppCompilerPathBuf;
        config.pythonPath = pythonPathBuf;
        config.loaded = true;
        config.loadError.clear();

        if (!saveConfig(config))
            config.loadError = "Failed to save config file!";

        client.setConfig(config);
        // Re-register tools with new workspace path
        setupTools();
        // Sync tool paths to agent so buildChatRequest injects them
        agent->setToolPaths(config.cppCompilerPath, config.pythonPath);
        agent->setRequestParams(config.maxTokens, config.temperature);
        needsConfig = false;

        // Connection is tested on the first API call, no blocking health check
        ImGui::CloseCurrentPopup();
        config.loadError.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();

    if (!config.loadError.empty())
        ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "%s", config.loadError.c_str());
    ImGui::EndPopup();
}

