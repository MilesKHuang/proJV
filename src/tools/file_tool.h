#pragma once
#include "tool_paths.h"
#include <string>

// All file tools take one ToolPaths: the set of roots plus which are writable.
// It replaced a bare `const std::string& workspacePath`, which could only express
// "one directory, fully writable".
void registerFileTools(class ToolRegistry& registry, const ToolPaths& paths);

// Shared path guard used by every file-touching tool (and by view_image's
// lambda). Resolves `rawPath`, rejects escapes, refuses writes under a read-only
// root, and returns the resolved absolute path via `outResolved`. Returns "" on
// success, or the error text to hand back to the model.
std::string validateToolPath(const ToolPaths& paths, const std::string& rawPath, bool forWrite,
                             std::string& outResolved);

// True for VCS / build / dependency directories that every recursive traversal
// must skip. One list, shared by list_dir, grep_files and file_search.
bool isSkippedDirName(const std::string& dirName);

// Split a skill `file_requests` entry into path + optional line range.
// Accepts "a.cpp", "a.cpp#L120", "a.cpp#L120-L480" (case-insensitive 'L').
// `startLine` / `endLine` are 0 when absent. Returns false for an empty spec.
bool parseFileRequestSpec(const std::string& spec, std::string& outPath, int& startLine,
                          int& endLine);
