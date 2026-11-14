#pragma once
#include "tool_paths.h"
#include <string>

// Register the shell-based tools into the given registry.
// File tools live in file_tool.h (read/write/list/grep) and format_tool.h --
// this header used to redeclare registerFileTools as well, which is exactly the
// "two copies drift apart" pattern that broke md_file on Linux.
//
// Every tool takes the same ToolPaths (see tool_paths.h): the shell's cwd is
// paths.workspace(), the rest is the shared guard. The default keeps the old
// no-argument call working, and an empty ToolPaths means "no restriction".
class IProcessRunner;
class TaskRegistry;
void registerShellTool(class ToolRegistry& registry, const ToolPaths& paths = ToolPaths(),
                       IProcessRunner* procRunner = nullptr, TaskRegistry* background = nullptr);
// task_output: read a background task's output (see core/task_registry.h).
void registerTaskOutputTool(class ToolRegistry& registry, TaskRegistry* tasks);
void registerEditFileTool(class ToolRegistry& registry, const ToolPaths& paths = ToolPaths());
void registerFileSearchTool(class ToolRegistry& registry, const ToolPaths& paths = ToolPaths());
