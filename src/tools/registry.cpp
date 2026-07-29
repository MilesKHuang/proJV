#include "registry.h"
#include <cstdio>

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

void ToolRegistry::cancelAll() {
    // Only set the flag -- shell_tool's polling loop will detect it
    // and use TerminateJobObject to kill the entire process tree.
    // Do NOT call TerminateProcess here: it kills only cmd.exe and
    // leaves orphan children holding stdout pipe, causing reader.join()
    // to block forever in execCommand().
    cancelRequested_.store(true, std::memory_order_release);
}

bool ToolRegistry::isCancelled() const {
    return cancelRequested_.load(std::memory_order_acquire);
}

void ToolRegistry::resetCancel() {
    cancelRequested_.store(false, std::memory_order_release);
}
