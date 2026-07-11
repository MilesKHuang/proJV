#include "storage_queue.h"
#include "debug_log.h"

StorageWriteQueue::StorageWriteQueue() = default;

StorageWriteQueue::~StorageWriteQueue() {
    if (running_) stop();
}

void StorageWriteQueue::start() {
    if (running_) return;
    running_ = true;
    worker_ = std::thread(&StorageWriteQueue::workerLoop, this);
    debugLog("[WriteQueue] Worker thread started");
}

void StorageWriteQueue::enqueue(std::function<void()> fn) {
    if (!running_) {
        // Fallback: execute synchronously if worker isn't running.
        // This handles initialization-phase writes before start().
        debugLog("[WriteQueue] Not running, executing synchronously");
        fn();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(std::move(fn));
    }
    cv_.notify_one();
}

void StorageWriteQueue::flush() {
    std::unique_lock<std::mutex> lock(mutex_);
    // Wait for queue empty AND no task in flight (R4: cv-notify, not timeout).
    // doneCv_ is notified by worker after completing each task.
    doneCv_.wait(lock, [this]() {
        return queue_.empty() && !taskInFlight_.load(std::memory_order_acquire);
    });
}

void StorageWriteQueue::stop() {
    if (!running_) return;
    running_ = false;
    cv_.notify_one();
    // Wait for current inflight task to complete before joining
    {
        std::unique_lock<std::mutex> lock(mutex_);
        doneCv_.wait(lock, [this]() {
            return !taskInFlight_.load(std::memory_order_acquire);
        });
    }
    if (worker_.joinable()) worker_.join();
    debugLog("[WriteQueue] Worker thread stopped");
}

void StorageWriteQueue::workerLoop() {
    while (running_) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() { return !queue_.empty() || !running_; });
            if (!running_ && queue_.empty()) return;
            task = std::move(queue_.front());
            queue_.pop();
            taskInFlight_.store(true, std::memory_order_release);
        }
        // Execute the write operation outside the lock
        try {
            task();
        } catch (const std::exception& e) {
            debugLogf("[WriteQueue] Exception in write task: %s", e.what());
        }
        // Signal flush() that this task is complete
        {
            std::lock_guard<std::mutex> lock(mutex_);
            taskInFlight_.store(false, std::memory_order_release);
        }
        doneCv_.notify_all();
    }
}