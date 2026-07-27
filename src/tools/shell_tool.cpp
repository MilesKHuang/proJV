// shell_tool.cpp -- exec_shell via IProcessRunner
#include "shell_tool.h"
#include "registry.h"
#include "json_utils.h"
#include "platform/iprocess_runner.h"
#include <cstdlib>
#include <cstdio>
#include <string>
#include <filesystem>

static constexpr const char* TOOL_EXEC_SHELL_DESC_BASE =
    "Execute a shell command and return its output. ";
static constexpr const char* TOOL_EXEC_SHELL_DESC_FALLBACK =
    "Use for running programs, scripts, and diagnostics.";
static constexpr const char* TOOL_PARAM_COMMAND = "The shell command to execute";
static constexpr const char* TOOL_PARAM_STDIN =
    "Optional: content to pipe to the command's stdin. "
    "Use with PowerShell: powershell -Command \"$input | Set-Content -Path 'file.cpp' -Encoding UTF8\"";

namespace fs = std::filesystem;

static bool isPathWithinWorkspace(const std::string& path, const std::string& ws) {
    if (ws.empty()) return true;
    try {
        fs::path target = fs::absolute(path);
        fs::path base   = fs::absolute(ws);
        auto rel = fs::relative(target, base);
        return !rel.empty() && rel.native()[0] != '.';
    } catch (...) { return false; }
}

void registerShellTool(ToolRegistry& registry, const std::string& workspacePath,
                       IProcessRunner* procRunner)
{
    ToolDefinition def;
    def.name = "exec_shell";
    def.description = std::string(TOOL_EXEC_SHELL_DESC_BASE);
    if (!workspacePath.empty())
        def.description += "Commands run inside the workspace directory: " + workspacePath;
    else
        def.description += TOOL_EXEC_SHELL_DESC_FALLBACK;
    def.parameters = {
        {"command", "string", TOOL_PARAM_COMMAND, true},
        {"stdin", "string", TOOL_PARAM_STDIN, false}
    };

    registry.registerTool(def, [&registry, workspacePath, procRunner](const std::string& args) -> std::string {
        std::string cmd = extractStringArg(args, "command");
        std::string stdinContent = extractStringArg(args, "stdin");
        if (cmd.empty()) return "Error: Missing 'command' argument";

        ProcessConfig cfg;
        cfg.command      = cmd;
        cfg.workDir      = workspacePath;
        cfg.stdinContent = stdinContent;

        ProcessResult result = procRunner->Run(cfg);

        if (!result.stdout_.empty()) return result.stdout_;
        if (result.exitCode != 0)
            return "Exit code: " + std::to_string(result.exitCode);
        return "";
    });
}
