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

private:
    std::unordered_map<std::string, ToolDefinition> definitions;
    std::unordered_map<std::string, ToolExecutor> executors;
};
