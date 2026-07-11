#include "registry.h"

void ToolRegistry::registerTool(const ToolDefinition& def, ToolExecutor executor) {
    definitions[def.name] = def;
    executors[def.name] = executor;
}

std::vector<ToolDefinition> ToolRegistry::getToolDefinitions() const {
    std::vector<ToolDefinition> result;
    for (const auto& [name, def] : definitions) {
        result.push_back(def);
    }
    return result;
}

std::string ToolRegistry::execute(const std::string& name, const std::string& args) {
    auto it = executors.find(name);
    if (it == executors.end()) {
        return "Error: Unknown tool '" + name + "'";
    }
    try {
        return it->second(args);
    } catch (const std::exception& e) {
        return "Error executing " + name + ": " + e.what();
    }
}
