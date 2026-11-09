// proJV -- MCP child process control (three pipes, one process tree).
//
// Deliberately parallel to platform/iprocess_runner.h and deliberately NOT it:
//   - IProcessRunner is one child at a time, holds its lock until exit, and its
//     Cancel() means "the current child". MCP needs N long-lived children that
//     outlive a single call, so it must not borrow that lock (see the design
//     note in iprocess_runner.h).
//   - stdout and stderr get SEPARATE pipes here. The runner merges them because
//     its output is text for a human; MCP's stdout is a protocol stream, and one
//     log line in the middle of it is a framing bug.
//
// Everything in this layer is single-threaded on the calling side: there is no
// reader thread. Callers poll readAvailable() in a loop with their own deadline,
// which also lets them honour a cancel request every 100ms.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace mcp {

struct SpawnSpec {
    std::string command;                  // executable, resolved on PATH
    std::vector<std::string> args;        // argv, never a shell string
    std::map<std::string, std::string> env; // added to the inherited environment
    std::string workDir;                  // empty = inherit
};

// One spawned server. Handles stay open for the whole server lifetime.
struct ProcHandles {
#ifdef _WIN32
    void* hProc = nullptr;
    void* hJob = nullptr;   // Job Object: KILL_ON_JOB_CLOSE takes the tree down
    void* hInWr = nullptr;  // parent -> child stdin
    void* hOutRd = nullptr; // child stdout -> parent
    void* hErrRd = nullptr; // child stderr -> parent (log only, never parsed)
#else
    int pid = 0;
    int fdIn = -1;
    int fdOut = -1;
    int fdErr = -1;
#endif
    bool valid = false;
    bool eof = false; // stdout hit EOF (the server is gone or closed its end)
};

// Create the process with three pipes. No shell: argv goes straight to
// CreateProcess/execvp, because an intermediate cmd.exe would rewrite newlines
// and break framing. On Windows the child gets CREATE_NO_WINDOW, otherwise every
// start flashes a console window.
bool spawn(const SpawnSpec& spec, ProcHandles& h, std::string& err);

// Write one line (a '\n' is appended). Returns false when the pipe is gone.
bool writeLine(ProcHandles& h, const std::string& line, std::string& err);

// Send EOF: the documented way to tell a stdio MCP server we are done.
// There is no `shutdown` request in the protocol; closing stdin is the signal.
void closeStdin(ProcHandles& h);

// Non-blocking read of whatever is buffered. Appends stdout bytes to `out` and
// stderr bytes to `errOut`. Returns the number of bytes read (0 = nothing yet).
size_t readAvailable(ProcHandles& h, std::string& out, std::string& errOut);

bool alive(ProcHandles& h);

// Terminate the whole tree. `force` = skip the graceful kill step.
// Windows: TerminateJobObject (or TerminateProcess without a job).
// POSIX:   killpg(SIGTERM) then SIGKILL when force.
void terminate(ProcHandles& h, bool force);

// Close every handle, which also kills the tree on Windows (KILL_ON_JOB_CLOSE).
void closeAll(ProcHandles& h);

} // namespace mcp
