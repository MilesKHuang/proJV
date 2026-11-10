// proJV -- MCP server list: projv_files/mcp.json.
//
// A separate file from config.toml on purpose: config.toml holds proJV's own
// runtime parameters, while this is a list of external processes that can be
// hot-added, toggled and that may carry a server token. It also lives next to
// the exe (getProjvDir), exactly like config.toml / prompts / skills / sessions,
// which makes the `enabled` flag an install-wide setting rather than a
// per-project one.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace mcp {

struct McpServerConfig {
    std::string name;
    // What this server is for, in one line. REQUIRED in practice: it is the only
    // source of the server's purpose in the capability map (P3), so a missing one
    // leaves the model guessing what the server can do.
    std::string description;
    std::string command;
    std::vector<std::string> args;
    std::map<std::string, std::string> env;
    // "stateful" (legacy handshake) or "stateless" (modern, per-request _meta).
    std::string protocol = "stateful";
    bool trusted = false; // the only security switch: false = never spawned
    bool enabled = false; // must be turned on explicitly
    int timeoutMs = 120000;
};

// <exe dir>/projv_files/mcp.json (see getProjvDir).
std::string configPath();

// Create the file with {"servers": []} when it does not exist yet, mirroring
// ensureDefaultPrompts(). Returns false only when it exists but is unreadable.
bool ensureConfigFile(const std::string& path);

// Parse and validate. Returns false only when the file cannot be read or is not
// valid JSON -- a single bad entry is skipped with a log line and does NOT take
// the rest of the list down. Duplicate names: the first wins.
bool loadConfig(const std::string& path, std::vector<McpServerConfig>& out, std::string& err);

// Set one entry's `enabled` flag and write the file back, preserving every
// other key byte-for-byte at the JSON level (unknown fields survive).
bool saveEnabled(const std::string& path, const std::string& name, bool enabled, std::string& err);

} // namespace mcp
