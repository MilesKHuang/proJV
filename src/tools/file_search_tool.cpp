#include "registry.h"
#include "shell_tool.h"
#include "file_tool.h"      // validateToolPath + isSkippedDirName (shared guards)
#include "json_utils.h"
#include <string>

// -- Tool description strings (moved from prompts.h) --------------------
static constexpr const char* TOOL_FILE_SEARCH_DESC =
    "Find files by name, recursively. Supports '*' and '?'. Stays inside the "
    "workspace and skips build / VCS / dependency directories. Returns absolute "
    "paths, at most 50.";
static constexpr const char* TOOL_PARAM_PATTERN = "Filename pattern to search for (supports wildcards)";
static constexpr const char* TOOL_PARAM_SEARCH_PATH = "Base directory to search (default: workspace root)";
#include <filesystem>
#include <fstream>
#include <regex>

namespace fs = std::filesystem;

namespace {

// Escape everything regex-special, then turn the two wildcards into regex.
// Without this, a pattern like "config.toml" also matches "configXtoml".
std::string wildcardToRegex(const std::string& pattern) {
    std::string out;
    out.reserve(pattern.size() * 2);
    for (char c : pattern) {
        switch (c) {
            case '*': out += ".*"; break;
            case '?': out += "."; break;
            case '.': case '+': case '(': case ')': case '[': case ']':
            case '{': case '}': case '^': case '$': case '|': case '\\':
                out += '\\';
                out += c;
                break;
            default: out += c;
        }
    }
    return out;
}

} // namespace

void registerFileSearchTool(ToolRegistry& registry, const std::string& workspacePath) {
    ToolDefinition def;
    def.name = "file_search";
    def.description = TOOL_FILE_SEARCH_DESC;
    def.parameters = {
        {"pattern", "string", TOOL_PARAM_PATTERN, true},
        {"path", "string", TOOL_PARAM_SEARCH_PATH, false}
    };

    registry.registerTool(def, [workspacePath](const std::string& args) -> std::string {
        const std::string pattern = extractStringArg(args, "pattern");
        std::string path = extractStringArg(args, "path");
        if (pattern.empty()) return "Error: Missing 'pattern' argument";
        if (path.empty()) path = workspacePath.empty() ? std::string(".") : workspacePath;

        // Same guard as every other file tool: no escaping the workspace.
        std::string resolved;
        const std::string err = validateToolPath(path, workspacePath, resolved);
        if (!err.empty()) return err;

        std::error_code ec;
        if (!fs::is_directory(resolved, ec))
            return "Error: Not a directory: " + resolved;

        try {
            const std::regex re(wildcardToRegex(pattern), std::regex::ECMAScript | std::regex::icase);
            std::string result;
            int count = 0;
            bool truncated = false;

            for (auto it = fs::recursive_directory_iterator(
                     resolved, fs::directory_options::skip_permission_denied, ec);
                 it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) { ec.clear(); continue; }
                const fs::directory_entry& entry = *it;

                std::error_code ec2;
                if (entry.is_directory(ec2)) {
                    // Skip the same junk directories grep_files and list_dir skip.
                    if (isSkippedDirName(entry.path().filename().string()))
                        it.disable_recursion_pending();
                    continue;
                }
                if (!entry.is_regular_file(ec2)) continue;

                if (!std::regex_match(entry.path().filename().string(), re)) continue;
                result += entry.path().string() + "\n";
                if (++count >= 50) { truncated = true; break; }
            }

            if (count == 0) return "No files found matching: " + pattern;
            if (truncated) result += "... and more (truncated at 50 results)";
            return result;
        } catch (const std::exception& e) {
            return "Error: " + std::string(e.what());
        }
    });
}
