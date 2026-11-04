// proJV -- per-tool-call capture of what a file-writing tool just changed.
//
// The four writing tools (write_file, edit_file, md_file, format_file) snapshot
// the file before and after their own write and hand the pair to record(). The
// Agent collects the entry right after ToolRegistry::execute() and hangs the
// unified diff on the tool message, so the TUI can show it in its own bubble and
// a replayed session shows it too. The diff is display-only: it is persisted
// with the tool row but never serialized into an API request.
#pragma once

#include <cstddef>
#include <string>

namespace diff_capture {

struct Entry {
    std::string path;    // display path (workspace-relative when possible)
    std::string unified; // unified diff; empty means "nothing to show"
};

// Record "this call changed this file". Nothing is recorded when the text is
// equivalent after line normalisation, or when either side looks binary.
// Deliberately thread_local, not a registry member: skill phases run SubAgent
// workers on parallel threads that share one ToolRegistry and each call
// tools_->execute(), so a member would let two workers overwrite each other.
// Never throws -- a capture failure must not change what the tool returns.
void record(const std::string& path, const std::string& before, const std::string& after);

// Hand the current thread's entry over and clear it.
Entry take();

// Drop the current thread's entry (called before every tool execution).
void reset();

// Unified diff of two texts, or "" when there is nothing to show: identical
// content, or a NUL byte on either side (binary, never rendered).
//   contextLines -- unchanged lines kept around each change
//   maxLines     -- hard cap on emitted lines
//   maxChars     -- hard cap on emitted characters
std::string makeUnifiedDiff(const std::string& before, const std::string& after,
                            const std::string& path, int contextLines = 3, int maxLines = 200,
                            std::size_t maxChars = 8000);

// The path printed in the "--- a/" / "+++ b/" header: relative to the workspace
// with forward slashes when the file is inside it, else the path as given.
std::string displayPath(const std::string& resolved, const std::string& workspace);

// Read a whole file in binary mode. False when it cannot be opened, in which
// case `out` is left untouched (callers start from an empty string).
bool readFileIfExists(const std::string& path, std::string& out);

} // namespace diff_capture
