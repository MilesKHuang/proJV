// proJV -- background tasks (exec_shell background:true, /end_task).
//
// Why this is not IProcessRunner: that runner is serialized on purpose (one child,
// blocking, cancellable) because a second concurrent child would make Cancel()
// ambiguous. A background task is the opposite requirement -- detached, several at
// once, output kept for later. See iprocess_runner.h; do not merge the two.
//
// Design: the child writes its stdout+stderr straight to a per-task temp file
// instead of a pipe. A pipe nobody drains deadlocks the child as soon as the OS
// buffer fills (64 KB), and a build or a download fills it in milliseconds. With a
// file, reading is a seek-to-end and cannot block the child at all. The cost is
// disk I/O proportional to the output, which is what "background" is for.
//
// Lifetime: one monitor thread per task, always joined (never detached). Windows
// puts the child in a Job Object with KILL_ON_JOB_CLOSE, so a proJV crash takes the
// child with it; Linux uses its own process group and killpg.
#pragma once
#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class TaskRegistry;

namespace bg_task {

// 60 minutes, not the 30 used by exec_shell: downloads and full rebuilds routinely
// exceed half an hour, and a background task exists precisely so nobody waits on it.
constexpr int kBackgroundTimeoutMs = 3600000;
// Ring bound for what the dock and task_output read back.
constexpr size_t kKeepBytes = 100u * 1024u;

enum class State { Running, Done, Failed, Killed, TimedOut };

struct Snapshot {
    int id = 0;
    std::string command;
    State state = State::Running;
    int exitCode = 0;
    size_t outputBytes = 0;
    std::string lastLine; // latest non-empty output line, for the dock
    long long startedAtMs = 0;
    // The child's pid, for a host that samples resource use per task (the studio's
    // System panel). 0 when the platform could not report it or the task is over.
    int pid = 0;
};

// Latest non-empty line of `s` (trailing \r\n trimmed, cut to `maxCols` bytes at a
// UTF-8 boundary). Pure, used by the dock and by the tests.
std::string latestLine(const std::string& s, size_t maxCols);

const char* stateLabel(State s);

// The block the model reads once a task is over: one "[background] bg#N ..."
// line plus a task_output hint per id, in the order given. A batch (several tasks
// ended together) is ONE message with one entry each, never one message per task:
// that is what makes an idle wake cost a single model call.
//
// An id with no snapshot still gets its line. Dropping it would leave the model
// waiting forever for a task it started, so the report is never allowed to be
// silent about an id -- and this is pure, so the wording is testable.
std::string finishedReportText(const std::vector<int>& ids, const std::vector<Snapshot>& snaps);

} // namespace bg_task

class TaskRegistry {
  public:
    TaskRegistry();
    ~TaskRegistry();
    TaskRegistry(const TaskRegistry&) = delete;
    TaskRegistry& operator=(const TaskRegistry&) = delete;

    // Spawn `command` in `workDir`. Returns the task id (>0) or 0 on failure, with
    // `error` filled in.
    int start(const std::string& command, const std::string& workDir, std::string& error);

    // Kill one task (whole tree) or every task. Returns how many were signalled.
    bool end(int id, std::string& error);
    size_t endAll();

    // Read up to `tailLines` (<=0: kDefaultTailLines) trailing lines of output, or
    // the whole (bounded) output when fullOutput is true. Uses the same tail-first
    // shaper as exec_shell, so a 500 MB log never enters the context.
    std::string read(int id, int tailLines, bool fullOutput, std::string& error) const;

    std::vector<bg_task::Snapshot> snapshots() const;
    bool empty() const;
    size_t runningCount() const;

    // Ids that finished since the last call, oldest first. The turn loop turns each
    // one into a synthetic message so the model learns about it.
    std::vector<int> takeFinished();
    // Called by the monitor thread for every task that just finished -- exited,
    // timed out, or stopped by /end_task (a kill has to report too, or the model
    // waits forever for an id that will never speak again). Installed once, by the
    // owner, and run with NO registry lock held: the hook may call takeFinished()
    // or snapshots(). Keep it short -- it shares the thread that reaps every task.
    void setOnFinished(std::function<void(int)> cb);
    // Called by the turn loop's owner: shutdown() kills everything, joins the
    // monitor threads, and removes the temp files.
    void shutdown();

  private:
    struct Task;
    std::shared_ptr<Task> find(int id) const;
    void reaper();

    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<Task>> tasks_;
    std::vector<int> finished_;
    std::function<void(int)> onFinished_; // guarded by mutex_; copied out before the call
    std::thread reaper_;
    std::atomic<bool> stopping_{false};
};
