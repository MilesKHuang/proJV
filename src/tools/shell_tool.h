#pragma once
#include <string>

// Register the shell-based tools into the given registry.
// File tools live in file_tool.h (read/write/list/grep) and format_tool.h --
// this header used to redeclare registerFileTools as well, which is exactly the
// "two copies drift apart" pattern that broke md_file on Linux.
class IProcessRunner;
void registerShellTool(class ToolRegistry& registry, const std::string& workspacePath = "",
                       IProcessRunner* procRunner = nullptr);
void registerEditFileTool(class ToolRegistry& registry);
void registerFileSearchTool(class ToolRegistry& registry, const std::string& workspacePath = "");
