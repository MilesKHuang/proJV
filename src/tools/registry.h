#pragma once
#include "models.h"
#include <unordered_map>
#include <functional>
#include <string>
#include <atomic>

using ToolExecutor = std::function<std::string(const std::string& args)>;

class IProcessRunner;  // platform/iprocess_runner.h

class ToolRegistry {
public:
    void registerTool(const ToolDefinition& def, ToolExecutor executor);
    std::vector<ToolDefinition> getToolDefinitions() const;
    std::string execute(const std::string& name, const std::string& args);

    // Wire the platform process runner so cancelAll() can actually interrupt
    // a running subprocess (Agent::cancel() -> cancelAll() -> runner.Cancel()).
    void setProcessRunner(IProcessRunner* runner) { procRunner_ = runner; }

    // Cancel support: interrupt executing tool subprocesses
    void cancelAll();
    bool isCancelled() const;
    void resetCancel();

private:
    std::unordered_map<std::string, ToolDefinition> definitions;
    std::unordered_map<std::string, ToolExecutor> executors;
    std::atomic<bool> cancelRequested_{false};
    IProcessRunner* procRunner_ = nullptr;
};
