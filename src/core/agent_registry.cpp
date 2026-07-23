#include "agent_registry.h"
#include "prompts.h"
#include "tool_list_parser.h"
#include "config.h"
#include "debug_log.h"
#include "json.hpp"
#include <fstream>
#include <filesystem>
#include <algorithm>

bool AgentRegistry::loadWorkflow(const std::string& jsonPath) {
    std::ifstream ifs(jsonPath);
    if (!ifs) {
        debugLogf("[AgentRegistry] Cannot open workflow: %s", jsonPath.c_str());
        return false;
    }
    nlohmann::json j;
    try {
        ifs >> j;
    } catch (const std::exception& e) {
        debugLogf("[AgentRegistry] JSON parse error in %s: %s", jsonPath.c_str(), e.what());
        return false;
    }
    WorkflowConfig cfg;
    cfg.version = j.value("version", "1.0");
    cfg.description = j.value("description", "");

    // Parse main_agent
    if (!j.contains("main_agent") || !j["main_agent"].is_object()) {
        debugLog("[AgentRegistry] Workflow missing 'main_agent'");
        return false;
    }
    auto& ma = j["main_agent"];
    cfg.mainAgent.promptFile = ma.value("prompt_file", "supervisor.md");
    cfg.mainAgent.model = ma.value("model", "");
    // Load prompt content
    cfg.mainAgent.promptContent = loadPromptFile(cfg.mainAgent.promptFile);
    if (cfg.mainAgent.promptContent.empty()) {
        debugLogf("[AgentRegistry] Warning: main_agent prompt file '%s' is empty or missing",
                  cfg.mainAgent.promptFile.c_str());
    }

    // Parse subagents
    if (j.contains("subagents") && j["subagents"].is_array()) {
        for (auto& sa : j["subagents"]) {
            SubAgentConfig sc;
            sc.name = sa.value("name", "");
            sc.display = sa.value("display", sc.name);
            sc.promptFile = sa.value("prompt_file", "");
            sc.description = sa.value("description", "");
            sc.model = sa.value("model", "deepseek-v4-flash");
            sc.visible = sa.value("visible", true);

            // Load prompt content
            sc.promptContent = loadPromptFile(sc.promptFile);
            if (sc.promptContent.empty()) {
                debugLogf("[AgentRegistry] Warning: subagent '%s' prompt file '%s' not found",
                          sc.name.c_str(), sc.promptFile.c_str());
                continue;
            }
            // Parse tools available from prompt
            sc.allowedTools = parseAvailableTools(sc.promptContent);
            cfg.subagents.push_back(std::move(sc));
        }
    }

    // Validate: at least one visible subagent
    bool hasVisible = false;
    for (auto& s : cfg.subagents) {
        if (s.visible) { hasVisible = true; break; }
    }
    if (!hasVisible && !cfg.subagents.empty()) {
        debugLog("[AgentRegistry] Warning: no visible subagents in workflow");
    }

    config_ = std::move(cfg);
    debugLogf("[AgentRegistry] Loaded workflow: %s (%zu subagents)",
              jsonPath.c_str(), config_.subagents.size());
    return true;
}

std::vector<SubAgentConfig> AgentRegistry::getUserVisibleTypes() const {
    std::vector<SubAgentConfig> result;
    for (auto& s : config_.subagents) {
        if (s.visible) result.push_back(s);
    }
    return result;
}

std::optional<SubAgentConfig> AgentRegistry::findType(const std::string& name) const {
    for (auto& s : config_.subagents) {
        if (s.name == name) return s;
    }
    return std::nullopt;
}

// Built-in preset content for coding.json
static constexpr const char* PRESET_CODING_JSON =
R"({
  "version": "1.0",
  "description": "Default coding workflow -- full development cycle with analysis, design, implementation, and testing",

  "main_agent": {
    "prompt_file": "supervisor.md",
    "model": ""
  },

  "subagents": [
    {
      "name": "coder",
      "display": "Coder",
      "prompt_file": "coder.md",
      "description": "Code implementation, compilation, debugging",
      "model": "deepseek-v4-flash",
      "visible": true
    },
    {
      "name": "designer",
      "display": "Designer",
      "prompt_file": "designer.md",
      "description": "Architecture design documents",
      "model": "deepseek-v4-flash",
      "visible": true
    },
    {
      "name": "analyzer",
      "display": "Analyzer",
      "prompt_file": "analyzer.md",
      "description": "Code analysis, call tracing, diagram generation",
      "model": "deepseek-v4-flash",
      "visible": true
    },
    {
      "name": "tester",
      "display": "Tester",
      "prompt_file": "tester.md",
      "description": "Test writing, compilation, execution, reporting",
      "model": "deepseek-v4-flash",
      "visible": true
    }
  ]
})";

std::string AgentRegistry::buildAvailableSubAgentsMessage() const {
    auto visible = getUserVisibleTypes();
    if (visible.empty()) return "";
    std::string msg = "[AVAILABLE SUB-AGENTS] You can delegate tasks to these types via delegate_task (use agent_type parameter):\n";
    for (auto& s : visible) {
        msg += "  - " + s.name + ": " + s.description;
        if (!s.allowedTools.empty()) {
            msg += " (tools: ";
            for (size_t i = 0; i < s.allowedTools.size(); ++i) {
                if (i > 0) msg += ", ";
                msg += s.allowedTools[i];
            }
            msg += ")";
        }
        msg += "\n";
    }
    return msg;
}

std::string AgentRegistry::generatePresetWorkflow(const std::string& dirPath) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(dirPath, ec);

    std::string jsonPath = dirPath + "/coding.json";
    if (fs::exists(jsonPath, ec)) {
        debugLogf("[AgentRegistry] Preset workflow already exists: %s", jsonPath.c_str());
        return jsonPath;
    }

    std::ofstream ofs(jsonPath);
    if (!ofs) {
        debugLogf("[AgentRegistry] Failed to create preset workflow: %s", jsonPath.c_str());
        return "";
    }
    ofs << PRESET_CODING_JSON;
    debugLogf("[AgentRegistry] Generated preset workflow: %s", jsonPath.c_str());
    return jsonPath;
}
