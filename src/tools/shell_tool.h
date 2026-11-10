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
void registerShellTool(class ToolRegistry& registry, const ToolPaths& paths = ToolPaths(),
                       IProcessRunner* procRunner = nullptr);
void registerEditFileTool(class ToolRegistry& registry, const ToolPaths& paths = ToolPaths());
void registerFileSearchTool(class ToolRegistry& registry, const ToolPaths& paths = ToolPaths());
