// proJV TUI -- approval dialog logic (pure functions, 1:1 with legacy dialog).
#pragma once

#include <string>
#include <vector>

namespace approval_logic {

// Extract the display command from a tool-call arguments JSON string.
// Mirrors: try { json::parse(args).value("command","") } catch { raw args }.
std::string extractCommand(const std::string& argumentsJson);

// Extract the list of file paths a delete-style command would remove.
// Mirrors the keyword scan + tokenization in renderToolApprovalDialog.
std::vector<std::string> extractDeleteFiles(const std::string& command);

} // namespace approval_logic
