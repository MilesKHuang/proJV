#pragma once
#include <queue>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>

// --- Serialized write queue for SQLite ---------------------------------
// Writes go through a single worker thread so the SQLite write connection
// is never accessed concurrently. Enqueue returns immediately.
class StorageWriteQueue {
public:
    StorageWriteQueue();
    ~StorageWriteQueue();

    // Non-copyable / non-movable
    StorageWriteQueue(const StorageWriteQueue&) = delete;
    StorageWriteQueue& operator=(const StorageWriteQueue&) = delete;

    // Start the worker thread. Must be called before enqueue.
    void start();

    // Enqueue a write operation to be executed on the worker thread.
    // If the queue is not running, executes immediately (synchronous fallback).
    void enqueue(std::function<void()> fn);

    // Flush all pending writes and stop the worker thread.
    // Blocks until the worker finishes its current task and the thread joins.
    void stop();

    // Block until all currently-enqueued writes have completed.
    // Fixed (R4): waits for both queue empty AND no inflight task.
    void flush();

    bool isRunning() const { return running_; }

private:
    void workerLoop();

    std::queue<std::function<void()>> queue_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;       // notify worker of new tasks / stop
    std::condition_variable doneCv_;   // notify flush() of task completion
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> taskInFlight_{false};  // true when worker is executing a task
};