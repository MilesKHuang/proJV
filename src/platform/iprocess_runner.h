// proJV -- Process abstraction interface
// Windows: CreateProcess + Job Object  /  Linux: fork/exec + setpgid
#pragma once
#include <string>
#include <map>
#include <atomic>

// Unified subprocess timeout policy, shared by every execution path
// (exec_shell and pytool). Any tool that runs a subprocess must use the
// same default and must never exceed the same cap.
constexpr int kDefaultProcessTimeoutMs = 300000; // 5 minutes
constexpr int kMaxProcessTimeoutMs     = 600000; // 10 minutes

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
    virtual ProcessResult Run(const ProcessConfig& cfg) = 0;

    // Signal the running process to terminate.
    // Windows: TerminateJobObject / Linux: killpg(SIGTERM) -> SIGKILL.
    virtual void Cancel() = 0;

    // True while a process is executing.
    virtual bool IsRunning() const = 0;
};

// Factory: platform-specific implementation
IProcessRunner* CreateProcessRunner();
