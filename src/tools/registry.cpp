#include "registry.h"
#include "shell_tool.h"
#include "file_tool.h"
#include "md_file_tool.h"
#include "web_tools.h"

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

bool ToolRegistry::hasTool(const std::string& name) const {
    return definitions.find(name) != definitions.end();
}

void registerToolByName(ToolRegistry& registry, const std::string& name,
                        const std::string& workspacePath) {
    if (registry.hasTool(name)) return; // already registered

    if (name == "read_file" || name == "write_file") {
        registerFileTools(registry, workspacePath);
    } else if (name == "edit_file") {
        registerEditFileTool(registry);
    } else if (name == "exec_shell") {
        registerShellTool(registry, workspacePath);
    } else if (name == "grep_files" || name == "file_search") {
        registerFileSearchTool(registry);
    } else if (name == "web_search") {
        registerSearchTool(registry);
    } else if (name == "fetch_url") {
        registerFetchTool(registry);
    } else if (name == "md_file") {
        registerMdFileTool(registry, workspacePath);
    }
    // diagram_tool - requires PythonToolManager, not registered here
    // update_todo - registered by Agent constructor, not here
    // delegate_task - registered by App, not here
}
