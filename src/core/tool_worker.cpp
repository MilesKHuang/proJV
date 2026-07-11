#include "tool_worker.h"
#include "debug_log.h"
#include <algorithm>

// --- Lifecycle -----------------------------------------------------------

void ToolWorkerManager::setExecutor(ToolExecutor executor) {
    std::lock_guard<std::mutex> lock(launchMutex_);
    executor_ = std::move(executor);
    debugLog("[ToolWorker] Executor set");
}

void ToolWorkerManager::launch(const std::vector<ToolCall>& calls, ResultCallback onDone,
                                ProgressCallback onProgress) {
    debugLogf("[ToolWorker] launch: %zu tool(s), acquiring mutex", calls.size());
    {
        std::lock_guard<std::mutex> lock(launchMutex_);
        stopFlag_.store(true, std::memory_order_release);
        if (worker_.joinable()) {
            debugLog("[ToolWorker] launch: detaching old worker");
            worker_.detach();
        }
        stopFlag_.store(false, std::memory_order_release);
        busy_.store(true, std::memory_order_release);
        onProgress_ = std::move(onProgress);
        debugLog("[ToolWorker] launch: creating new thread");
        worker_ = std::thread(&ToolWorkerManager::workerLoop, this, calls, std::move(onDone));
        debugLog("[ToolWorker] launch: thread created OK");
    }
}

void ToolWorkerManager::requestCancel() {
    debugLog("[ToolWorker] requestCancel");
    stopFlag_.store(true, std::memory_order_release);
}

void ToolWorkerManager::cancelAndJoin() {
    debugLog("[ToolWorker] cancelAndJoin (non-blocking)");
    stopFlag_.store(true, std::memory_order_release);
    busy_.store(false, std::memory_order_release);
}

void ToolWorkerManager::stop() {
    debugLog("[ToolWorker] stop: joining worker (blocking)");
    stopFlag_.store(true, std::memory_order_release);
    if (worker_.joinable()) {
        debugLog("[ToolWorker] stop: joining...");
        worker_.join();
        debugLog("[ToolWorker] stop: joined");
    }
    busy_.store(false, std::memory_order_release);
}

// --- Worker loop ---------------------------------------------------------

void ToolWorkerManager::workerLoop(std::vector<ToolCall> calls, ResultCallback onDone) {
    debugLogf("[ToolWorker] workerLoop START: %zu tool(s)", calls.size());

    std::vector<ToolResult> results;
    results.reserve(calls.size());

    for (size_t i = 0; i < calls.size(); ++i) {
        if (stopFlag_.load(std::memory_order_acquire)) {
            debugLogf("[ToolWorker] workerLoop: stopFlag at tool %zu/%zu", i, calls.size());
            size_t remaining = calls.size() - i;
            if (remaining > 0) {
                ToolResult cancelResult;
                cancelResult.toolCallId = calls[i].id;
                cancelResult.name       = calls[i].name;
                cancelResult.content    = "[Cancelled: " + std::to_string(remaining)
                                        + " tool(s) skipped]";
                cancelResult.isError    = false;
                results.push_back(cancelResult);
            }
            break;
        }

        // R4: 通知外部（Agent）当前工具执行进度（UI 侧通过 getStatus() 快照读取）
        if (onProgress_) {
            onProgress_(static_cast<int>(i + 1), static_cast<int>(calls.size()), calls[i].name);
        }

        debugLogf("[ToolWorker] workerLoop: executing [%zu/%zu] %s", i+1, calls.size(), calls[i].name.c_str());
        ToolResult result = executor_(calls[i]);
        debugLogf("[ToolWorker] workerLoop: [%zu/%zu] %s done (%zu bytes)",
            i+1, calls.size(), calls[i].name.c_str(), result.content.size());
        recordResult(result);
        results.push_back(result);
    }

    debugLogf("[ToolWorker] workerLoop DONE: %zu/%zu results, calling onDone", results.size(), calls.size());
    if (onDone) {
        onDone(results);
        debugLog("[ToolWorker] workerLoop: onDone returned");
    }
    busy_.store(false, std::memory_order_release);
}

// --- Error detection -----------------------------------------------------

void ToolWorkerManager::recordResult(const ToolResult& result) {
    std::lock_guard<std::mutex> lock(resultsMutex_);
    if (recentResults_.size() >= kMaxRecent) {
        recentResults_.pop_front();
    }
    recentResults_.push_back(result);
}

static bool isErrorResult(const ToolResult& r) {
    // Use the explicit isError flag set by the tool executor,
    // NOT substring matching on content — source code routinely
    // contains "Error:" and "Missing", causing false positives.
    return r.isError;
}

int ToolWorkerManager::countConsecutiveToolErrors() const {
    std::lock_guard<std::mutex> lock(resultsMutex_);
    int count = 0;
    for (auto it = recentResults_.rbegin(); it != recentResults_.rend(); ++it) {
        if (isErrorResult(*it)) {
            ++count;
        } else {
            break;
        }
    }
    return count;
}

int ToolWorkerManager::countRecentFailures(const std::string& toolName) const {
    std::lock_guard<std::mutex> lock(resultsMutex_);
    int count = 0;
    for (const auto& r : recentResults_) {
        if (r.name == toolName && isErrorResult(r)) {
            ++count;
        }
    }
    return count;
}
