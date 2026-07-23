#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>

// Configuration for one sub-agent type from workflow JSON
struct SubAgentConfig {
    std::string name;            // "coder", "designer", ...
    std::string display;         // "Coder", "Designer", ...
    std::string promptFile;     // "coder.md"
    std::string promptContent;  // loaded prompt text
    std::string description;    // short description
    std::string model;          // model to use (default flash)
    bool visible = true;        // shown in UI
    std::vector<std::string> allowedTools; // from "tools available:" line
};

// Configuration for the main agent (Supervisor) from workflow JSON
struct MainAgentConfig {
    std::string promptFile;     // "supervisor.md"
    std::string model;          // empty = use config.toml default
    std::string promptContent;  // loaded prompt text
};

// Complete workflow configuration
struct WorkflowConfig {
    std::string version;
    std::string description;
    MainAgentConfig mainAgent;
    std::vector<SubAgentConfig> subagents;
};

// Manages available sub-agent types, loaded from workflow JSON files.
class AgentRegistry {
public:
    // Load a workflow JSON file. On success, replaces current config.
    // Returns true on success, false on failure (current config preserved).
    bool loadWorkflow(const std::string& jsonPath);

    // Get all user-visible sub-agent types from the current workflow.
    std::vector<SubAgentConfig> getUserVisibleTypes() const;

    // Find a sub-agent type by name. Returns nullopt if not found.
    std::optional<SubAgentConfig> findType(const std::string& name) const;

    // Get the main agent config from the current workflow.
    const MainAgentConfig& getMainAgentConfig() const { return config_.mainAgent; }

    // Get the full workflow config.
    const WorkflowConfig& getConfig() const { return config_; }

    // Build a system message listing available sub-agent types for the Supervisor prompt.
    std::string buildAvailableSubAgentsMessage() const;

    // Generate a preset coding.json workflow file if it doesn't exist.
    // Returns the path to the JSON file (whether newly created or existing).
    static std::string generatePresetWorkflow(const std::string& dirPath);

private:
    WorkflowConfig config_;
};
