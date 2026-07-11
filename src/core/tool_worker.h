#pragma once
#include "client/models.h"
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
#include <deque>
#include <functional>
#include <string>

// --- ToolWorkerManager -------------------------------------------------
// RAII-managed tool execution thread with:
//   - Single background worker (no detach, destructor joins)
//   - Mutex-guarded launch/cancel
//   - Recent-result cache for error detection (no session.messages scan)
//   - Result callback to Agent core (manager never touches HTTP / session)
//
// Usage:
//   ToolWorkerManager mgr;
//   mgr.setExecutor([](const ToolCall& c) { return executeTool(c); });
//   mgr.launch(calls, [&agent](auto& results) { agent.onToolResults(results); });
class ToolWorkerManager {
public:
    using ToolExecutor = std::function<ToolResult(const ToolCall&)>;
    using ResultCallback = std::function<void(const std::vector<ToolResult>&)>;
    using ProgressCallback = std::function<void(int current, int total, const std::string& toolName)>;

    ToolWorkerManager() = default;
    ~ToolWorkerManager() { stop(); }

    // Non-copyable, non-movable
    ToolWorkerManager(const ToolWorkerManager&) = delete;
    ToolWorkerManager& operator=(const ToolWorkerManager&) = delete;
    ToolWorkerManager(ToolWorkerManager&&) = delete;
    ToolWorkerManager& operator=(ToolWorkerManager&&) = delete;

    // --- Lifecycle ---------------------------------------------------

    // Set the tool executor (called from worker thread for each tool call).
    void setExecutor(ToolExecutor executor);

    // Launch tool execution on background thread.
    // - Cancels any currently-running worker (blocks until it finishes).
    // - onDone is called from the worker thread when all tools complete.
    void launch(const std::vector<ToolCall>& calls, ResultCallback onDone,
                ProgressCallback onProgress = nullptr);

    // Request cancellation (stopTools flag). Does NOT block.
    void requestCancel();

    // Cancel and join. Blocks until the worker thread exits.
    void cancelAndJoin();

    // Stop and join (destructor helper).
    void stop();

    // True while a tool worker is executing.
    bool isBusy() const { return busy_.load(std::memory_order_acquire); }

    // True if cancellation was requested.
    bool isCancelled() const { return stopFlag_.load(std::memory_order_acquire); }

    // --- Error detection (uses recent-results cache) -----------------

    // Count consecutive tool errors in the recent result cache.
    // A result is considered an error if its isError flag is set.
    // Returns the count of consecutive errors (scanning from most recent backward).
    int countConsecutiveToolErrors() const;

    // Count failures for a specific tool name across the recent cache.
    // Returns the number of results for `toolName` whose content indicates failure.
    int countRecentFailures(const std::string& toolName) const;

    // Record a completed tool result in the cache.
    void recordResult(const ToolResult& result);

private:
    void workerLoop(std::vector<ToolCall> calls, ResultCallback onDone);

    ToolExecutor executor_;
    ProgressCallback onProgress_;       // called per-tool from workerLoop

    std::thread    worker_;
    std::mutex     launchMutex_;        // serializes launch / cancelAndJoin
    std::atomic<bool> stopFlag_{false};  // signals cancellation to worker
    std::atomic<bool> busy_{false};     // true while worker is active

    // Recent results cache (circular buffer style, max 64 entries).
    static constexpr size_t kMaxRecent = 64;
    std::deque<ToolResult> recentResults_;
    mutable std::mutex     resultsMutex_;
};
