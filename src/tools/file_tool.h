#pragma once
#include <string>

void registerFileTools(class ToolRegistry& registry, const std::string& workspacePath);

// Shared workspace path guard used by every file-touching tool.
// Resolves `rawPath` against the workspace, rejects escapes, and returns the
// resolved absolute path via `outResolved`. Returns "" on success, or an error
// message to hand back to the model on violation.
std::string validateToolPath(const std::string& rawPath, const std::string& workspace,
                             std::string& outResolved);

// True for VCS / build / dependency directories that every recursive traversal
// must skip. One list, shared by list_dir, grep_files and file_search.
bool isSkippedDirName(const std::string& dirName);

// Split a skill `file_requests` entry into path + optional line range.
// Accepts "a.cpp", "a.cpp#L120", "a.cpp#L120-L480" (case-insensitive 'L').
// `startLine` / `endLine` are 0 when absent. Returns false for an empty spec.
bool parseFileRequestSpec(const std::string& spec, std::string& outPath, int& startLine,
                          int& endLine);
