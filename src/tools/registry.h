#pragma once
#include "models.h"
#include <unordered_map>
#include <functional>
#include <string>

using ToolExecutor = std::function<std::string(const std::string& args)>;

class ToolRegistry {
public:
    void registerTool(const ToolDefinition& def, ToolExecutor executor);
    std::vector<ToolDefinition> getToolDefinitions() const;
    std::string execute(const std::string& name, const std::string& args);
    bool hasTool(const std::string& name) const;

private:
    std::unordered_map<std::string, ToolDefinition> definitions;
    std::unordered_map<std::string, ToolExecutor> executors;
};

// Register a tool by name on the given registry using workspace path.
// Dispatches to the appropriate register* function based on tool name.
// Caller is responsible for managing the lifetime of captured references.
void registerToolByName(ToolRegistry& registry, const std::string& name,
                        const std::string& workspacePath);
