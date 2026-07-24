#pragma once
#include "models.h"
#include <unordered_map>
#include <functional>
#include <string>
#include <atomic>
#include <mutex>

using ToolExecutor = std::function<std::string(const std::string& args)>;

class ToolRegistry {
public:
    void registerTool(const ToolDefinition& def, ToolExecutor executor);
    std::vector<ToolDefinition> getToolDefinitions() const;
    std::string execute(const std::string& name, const std::string& args);

    // Cancel support: interrupt executing tool subprocesses
    void cancelAll();
    bool isCancelled() const;
    void resetCancel();

    // Access the active process handle (shell_tool writes this)
    std::mutex activeProcessMutex_;
    void* activeProcessHandle_ = nullptr;

private:
    std::unordered_map<std::string, ToolDefinition> definitions;
    std::unordered_map<std::string, ToolExecutor> executors;
    std::atomic<bool> cancelRequested_{false};
};
