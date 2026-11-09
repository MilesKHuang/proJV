#include "registry.h"
#include "platform/iprocess_runner.h"
#include <cstdio>

void ToolRegistry::registerTool(const ToolDefinition& def, ToolExecutor executor) {
    definitions[def.name] = def;
    executors[def.name] = executor;
}

bool ToolRegistry::registerToolIfAbsent(const ToolDefinition& def, ToolExecutor executor) {
    if (definitions.find(def.name) != definitions.end()) return false;
    definitions[def.name] = def;
    executors[def.name] = std::move(executor);
    return true;
}

size_t ToolRegistry::unregisterByNamePrefix(const std::string& prefix) {
    size_t removed = 0;
    for (auto it = definitions.begin(); it != definitions.end();) {
        if (it->first.rfind(prefix, 0) == 0) {
            executors.erase(it->first);
            it = definitions.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
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
    // Set the shared cancel flag (Agent reads it to mark the tool result as
    // cancelled), then actually interrupt the running subprocess. Calling
    // IProcessRunner::Cancel() makes the runner's wait loop terminate the
    // whole process tree (TerminateJobObject on Windows / killpg on Linux)
    // instead of waiting for the timeout. Safe when idle: the runner resets
    // its cancel flag at the start of every Run().
    cancelRequested_.store(true, std::memory_order_release);
    if (procRunner_) procRunner_->Cancel();
}

bool ToolRegistry::isCancelled() const {
    return cancelRequested_.load(std::memory_order_acquire);
}

void ToolRegistry::resetCancel() {
    cancelRequested_.store(false, std::memory_order_release);
}
