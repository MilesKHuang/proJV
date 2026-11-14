// proJV -- task_output: read what a background task printed.
//
// The read side of exec_shell(background:true). Shaping is delegated to the same
// tail-first shaper as exec_shell, so a 400 MB build log never enters the context.
#include "shell_tool.h"

#include "core/task_registry.h"
#include "json_utils.h"
#include "registry.h"
#include "shell_output.h"

#include <string>

namespace {

constexpr const char* TASK_OUTPUT_DESC =
    "Read the output of a background task started with exec_shell(background:true). Returns "
    "the LAST lines by default (the verdict/error is at the end). Also reports whether the "
    "task is still running, its exit code, and where the output file is.";
constexpr const char* PARAM_ID = "Task id, the N from bg#N";
constexpr const char* PARAM_TAIL = "Optional. Trailing lines to keep (default 400, range 10-2000)";
constexpr const char* PARAM_FULL =
    "Optional (default false). Whole output instead of the tail -- still capped at 200 KB";

} // namespace

void registerTaskOutputTool(ToolRegistry& registry, TaskRegistry* tasks) {
    ToolDefinition def;
    def.name = "task_output";
    def.description = TASK_OUTPUT_DESC;
    def.parameters = {{"id", "number", PARAM_ID, true},
                      {"tail_lines", "number", PARAM_TAIL, false},
                      {"full_output", "boolean", PARAM_FULL, false}};

    registry.registerTool(def, [tasks](const std::string& args) -> std::string {
        if (!tasks) return "Error: background tasks are not available here";
        const int id = extractIntArg(args, "id", 0);
        if (id <= 0) return "Error: Missing 'id' argument";
        const int tail = extractIntArg(args, "tail_lines", shell_output::kDefaultTailLines);
        const bool full = extractBoolArg(args, "full_output", false);

        std::string err;
        const std::string body = tasks->read(id, tail, full, err);
        if (!err.empty()) return "Error: " + err;

        std::string header;
        for (const auto& s : tasks->snapshots()) {
            if (s.id != id) continue;
            header = "[bg#" + std::to_string(id) + "] " + bg_task::stateLabel(s.state) + " (exit " +
                     std::to_string(s.exitCode) + ", " + std::to_string(s.outputBytes) +
                     " bytes so far)\n";
            break;
        }
        if (body.empty()) return header + "(no output yet)";
        return header + body;
    });
}
