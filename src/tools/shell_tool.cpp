// shell_tool.cpp -- exec_shell via IProcessRunner
#include "shell_tool.h"
#include "shell_output.h"
#include "core/task_registry.h"
#include "registry.h"
#include "json_utils.h"
#include "platform/iprocess_runner.h"
#include <cstdlib>
#include <cstdio>
#include <string>

static constexpr const char* TOOL_EXEC_SHELL_DESC_BASE =
    "Execute a shell command and return its output. Long output is cut to the LAST "
    "400 lines (the tail carries the verdict: '2 errors generated', 'tests failed'); "
    "the cut is announced with the number of omitted lines. ";
static constexpr const char* TOOL_EXEC_SHELL_DESC_FALLBACK =
    "Use for running programs, scripts, and diagnostics.";
static constexpr const char* TOOL_PARAM_COMMAND = "The shell command to execute";
static constexpr const char* TOOL_PARAM_STDIN = "Optional: content to pipe to the command's stdin. "
                                                "Use with PowerShell: powershell -Command \"$input "
                                                "| Set-Content -Path 'file.cpp' -Encoding UTF8\"";
static constexpr const char* TOOL_PARAM_TIMEOUT_MS =
    "Optional total timeout in milliseconds (default 1800000 = 30 min, hard cap 1800000 = 30 min). "
    "Pass a smaller value only to fail faster.";
static constexpr const char* TOOL_PARAM_TAIL_LINES =
    "Optional. How many trailing lines to keep when the output is long (default 400, "
    "range 10-2000). Raise it when the conclusion you need sits further up; the omitted "
    "count is always reported.";
static constexpr const char* TOOL_PARAM_FULL_OUTPUT =
    "Optional (default false). Keep the whole output instead of the tail -- still capped at "
    "200 KB, because that is the context budget, not a display preference. Use it only when "
    "the tail is genuinely not enough; otherwise narrow the command with a pipe.";
static constexpr const char* TOOL_PARAM_BACKGROUND =
    "Optional (default false). Detach the command and return a task id immediately. Only for "
    "work over 10 minutes (big downloads, full builds) AND only when you really have "
    "something else to do while it runs; ordinary commands stay in the foreground. The task "
    "appears in the dock; read its output with task_output, stop it with /end_task.";

void registerShellTool(ToolRegistry& registry, const ToolPaths& paths, IProcessRunner* procRunner,
                       TaskRegistry* background) {
    ToolDefinition def;
    def.name = "exec_shell";
    def.description = std::string(TOOL_EXEC_SHELL_DESC_BASE);
    if (!paths.workspace().empty())
        def.description += "Commands run inside the workspace directory: " + paths.workspace();
    else
        def.description += TOOL_EXEC_SHELL_DESC_FALLBACK;
    def.parameters = {{"command", "string", TOOL_PARAM_COMMAND, true},
                      {"stdin", "string", TOOL_PARAM_STDIN, false},
                      {"timeout_ms", "number", TOOL_PARAM_TIMEOUT_MS, false},
                      {"tail_lines", "number", TOOL_PARAM_TAIL_LINES, false},
                      {"full_output", "boolean", TOOL_PARAM_FULL_OUTPUT, false},
                      {"background", "boolean", TOOL_PARAM_BACKGROUND, false}};

    registry.registerTool(
        def, [&registry, paths, procRunner, background](const std::string& args) -> std::string {
            std::string cmd = extractStringArg(args, "command");
            std::string stdinContent = extractStringArg(args, "stdin");
            if (cmd.empty()) return "Error: Missing 'command' argument";
            // background: no runner, no stdin, no output shaping here --
            // the task writes to its own file and task_output shapes it.
            if (extractBoolArg(args, "background", false)) {
                if (!background) return "Error: background tasks are not available here";
                if (!stdinContent.empty())
                    return "Error: stdin is not supported for background tasks";
                std::string err;
                const int id = background->start(cmd, paths.workspace(), err);
                if (id == 0) return "Error: cannot start background task: " + err;
                return "[background bg#" + std::to_string(id) + " started] " + cmd +
                       "\nIt runs detached; the dock shows its progress. Read the "
                       "output with task_output(" +
                       std::to_string(id) +
                       ", tail_lines), stop it with "
                       "/end_task " +
                       std::to_string(id) +
                       ". Do not poll it in a loop -- do other work and the "
                       "runtime will tell you when it ends.";
            }
            // The registry is wired with a runner at startup; a null here means
            // the platform has no runner (CreateProcessRunner() returned null) or
            // the wiring was skipped. Say so instead of dereferencing it.
            if (!procRunner) return "Error: no process runner available on this platform";

            int timeoutMs = extractIntArg(args, "timeout_ms", kProcessTimeoutMs);
            if (timeoutMs <= 0) timeoutMs = kProcessTimeoutMs;
            if (timeoutMs > kProcessTimeoutMs) timeoutMs = kProcessTimeoutMs;

            const int tailLines =
                extractIntArg(args, "tail_lines", shell_output::kDefaultTailLines);
            const bool fullOutput = extractBoolArg(args, "full_output", false);

            ProcessConfig cfg;
            cfg.command = cmd;
            cfg.workDir = paths.workspace();
            cfg.stdinContent = stdinContent;
            cfg.timeoutMs = timeoutMs;

            ProcessResult result = procRunner->Run(cfg);

            if (result.timedOut) {
                std::string msg =
                    "Error: exec_shell timed out after " + std::to_string(timeoutMs) + " ms";
                if (!result.stdout_.empty())
                    msg += "\nPartial output:\n" +
                           shell_output::apply(result.stdout_, tailLines, fullOutput).text;
                return msg;
            }
            if (result.cancelled) return "Error: exec_shell cancelled";
            if (!result.stdout_.empty()) {
                // Timeout partial output above goes through the same shaper:
                // a stuck build is exactly the case that produces 10 MB.
                return shell_output::apply(result.stdout_, tailLines, fullOutput).text;
            }
            if (result.exitCode != 0) return "Exit code: " + std::to_string(result.exitCode);
            return "";
        });
}
