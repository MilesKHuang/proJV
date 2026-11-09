#pragma once
#include "models.h"
#include <unordered_map>
#include <functional>
#include <string>
#include <atomic>

using ToolExecutor = std::function<std::string(const std::string& args)>;

class IProcessRunner; // platform/iprocess_runner.h

class ToolRegistry {
  public:
    void registerTool(const ToolDefinition& def, ToolExecutor executor);
    // Register only when the name is free. MCP uses this so an external server
    // can never silently take over a builtin: the mcp__<server>__<tool> prefix
    // makes a collision structurally impossible, but the guard is the cheap half
    // of that promise. Returns true when the tool was actually added.
    bool registerToolIfAbsent(const ToolDefinition& def, ToolExecutor executor);
    // Drop every tool whose name starts with `prefix`; returns how many went.
    // Used on a turn boundary when a server stops (see McpManager::syncTools).
    size_t unregisterByNamePrefix(const std::string& prefix);
    std::vector<ToolDefinition> getToolDefinitions() const;
    std::string execute(const std::string& name, const std::string& args);

    // Wire the platform process runner so cancelAll() can actually interrupt
    // a running subprocess (Agent::cancel() -> cancelAll() -> runner.Cancel()).
    void setProcessRunner(IProcessRunner* runner) { procRunner_ = runner; }

    // Cancel support: interrupt executing tool subprocesses
    void cancelAll();
    bool isCancelled() const;
    void resetCancel();

    // Turn-end request: a tool may ask the Agent to stop after the current tool
    // round (run_skill -- the skill runs as its own phase outside the turn, so
    // the model must not be asked for another round in between). The Agent
    // checks it once, right after the tool results are written, so the
    // assistant(tool_calls) -> tool pairing stays intact.
    void requestTurnEnd() { turnEndRequested_.store(true, std::memory_order_release); }
    bool consumeTurnEndRequest() {
        return turnEndRequested_.exchange(false, std::memory_order_acq_rel);
    }

  private:
    std::unordered_map<std::string, ToolDefinition> definitions;
    std::unordered_map<std::string, ToolExecutor> executors;
    std::atomic<bool> cancelRequested_{false};
    std::atomic<bool> turnEndRequested_{false};
    IProcessRunner* procRunner_ = nullptr;
};
