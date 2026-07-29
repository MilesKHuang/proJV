#pragma once
#include <string>

// Register shell and file tools into the given registry
class IProcessRunner;
void registerShellTool(class ToolRegistry& registry, const std::string& workspacePath = "",
                       IProcessRunner* procRunner = nullptr);
void registerFileTools(class ToolRegistry& registry, const std::string& workspacePath = "");
void registerEditFileTool(class ToolRegistry& registry);
// void registerGitTools(class ToolRegistry& registry);  // removed — git handled via shell
void registerFileSearchTool(class ToolRegistry& registry);