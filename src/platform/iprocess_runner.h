// proJV -- Process abstraction interface
// Windows: CreateProcess + Job Object  /  Linux: fork/exec + setpgid
#pragma once
#include <string>
#include <map>
#include <atomic>

// Unified subprocess timeout policy, shared by every execution path
// (exec_shell and pytool). Single source of truth: default == max, because
// long local inference and cold model loads legitimately run for tens of
// minutes. Callers may ask for LESS (fail faster), never more.
constexpr int kProcessTimeoutMs = 1800000; // 30 minutes

struct ProcessConfig {
    std::string command;            // shell command to execute
    std::string workDir;            // working directory (empty = inherit)
    std::string stdinContent;       // optional stdin data
    int         timeoutMs = 0;      // total wall-clock timeout; 0 = no timeout
    int         idleTimeoutMs = 0;  // max silence between output; 0 = disabled
    bool        inheritEnv = true;  // inherit parent environment
    std::map<std::string, std::string> extraEnv;
};

struct ProcessResult {
    std::string stdout_;
    std::string stderr_;
    int         exitCode  = 0;
    bool        timedOut  = false;
    bool        cancelled = false;
};

class IProcessRunner {
public:
    virtual ~IProcessRunner() = default;

    // Synchronous: blocks until process exits, times out, or is cancelled.
    //
    // DESIGN NOTE -- do not "fix" the serialization:
    //   proJV creates exactly ONE runner for the whole process (see
    //   CreateProcessRunner() and TuiApp::procRunner_), and Run() holds the
    //   runner-wide lock for the entire lifetime of the child process. That is
    //   intentional:
    //     - an external program cannot be relied on to receive ESC, so only one
    //       child may exist at a time;
    //     - Cancel() targets "the current process", which is only meaningful
    //       while a single process owns the runner;
    //     - a hung command must never be able to wedge a second caller.
    //   Anything that needs real concurrency (skill workers, file tools) must
    //   use the in-process tools, never the runner. Do not turn this into a
    //   pool, and do not drop the lock.
    virtual ProcessResult Run(const ProcessConfig& cfg) = 0;

    // Signal the running process to terminate.
    // Windows: TerminateJobObject / Linux: killpg(SIGTERM) -> SIGKILL.
    virtual void Cancel() = 0;

    // True while a process is executing.
    virtual bool IsRunning() const = 0;
};

// Factory: platform-specific implementation
IProcessRunner* CreateProcessRunner();
