#include "registry.h"
#include "shell_tool.h"
#include "json_utils.h"
#include <string>

// -- Tool description strings (moved from prompts.h) --------------------
static constexpr const char* TOOL_FILE_SEARCH_DESC =
    "Find files by name using pattern matching. Searches directory recursively.";
static constexpr const char* TOOL_PARAM_PATTERN = "Filename pattern to search for (supports wildcards)";
static constexpr const char* TOOL_PARAM_SEARCH_PATH = "Base directory to search (default: current directory)";
#include <filesystem>
#include <fstream>
#include <regex>

namespace fs = std::filesystem;

// Resolve relative paths to absolute to avoid CWD-dependent failures
static std::string resolveSearchPath(const std::string& path) {
    try {
        fs::path p(path);
        return fs::absolute(p).lexically_normal().string();
    } catch (...) {
        return path;
    }
}

void registerFileSearchTool(ToolRegistry& registry) {
    ToolDefinition def;
    def.name = "file_search";
    def.description = TOOL_FILE_SEARCH_DESC;
    def.parameters = {
        {"pattern", "string", TOOL_PARAM_PATTERN, true},
        {"path", "string", TOOL_PARAM_SEARCH_PATH, false}
    };

    registry.registerTool(def, [](const std::string& args) -> std::string {
        auto extractFn = [&](const std::string& key, const std::string& fallback) -> std::string {
            std::string val = extractStringArg(args, key);
            return val.empty() ? fallback : val;
        };

        std::string pattern = extractFn("pattern", "");
        std::string rawPath = extractFn("path", ".");

        if (pattern.empty()) return "Error: Missing 'pattern' argument";

        // Resolve relative paths to absolute to avoid CWD-dependent failures
        std::string searchPath = resolveSearchPath(rawPath);

        // Convert simple wildcard pattern to regex
        std::string regexPattern;
        for (char c : pattern) {
            if (c == '*') regexPattern += ".*";
            else if (c == '?') regexPattern += ".";
            else regexPattern += c;
        }

        try {
            std::regex re(regexPattern, std::regex::ECMAScript | std::regex::icase);
            std::string result;
            int count = 0;

            for (const auto& entry : fs::recursive_directory_iterator(searchPath,
                fs::directory_options::skip_permission_denied))
            {
                if (!entry.is_regular_file() && !entry.is_directory()) continue;
                auto name = entry.path().filename().string();
                if (std::regex_match(name, re)) {
                    result += entry.path().string() + "\n";
                    ++count;
                    if (count >= 50) {
                        result += "... and more (truncated at 50 results)";
                        break;
                    }
                }
            }

            if (count == 0) return "No files found matching: " + pattern;
            return result;
        } catch (const std::exception& e) {
            return "Error: " + std::string(e.what());
        }
    });
}
