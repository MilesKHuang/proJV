#include "registry.h"
#include "shell_tool.h"
#include "json_utils.h"
#include <fstream>

// -- Tool description strings (moved from prompts.h) --------------------
static constexpr const char* TOOL_EDIT_FILE_DESC =
    "Make a surgical text replacement in a file. "
    "Searches for `search` text and replaces it with `replace`. "
    "Handles both LF and CRLF line endings automatically. "
    "Use this for targeted edits instead of rewriting the whole file.";
static constexpr const char* TOOL_PARAM_EDIT_PATH = "Path to the file to edit";
static constexpr const char* TOOL_PARAM_SEARCH = "Text to search for (including whitespace and newlines)";
static constexpr const char* TOOL_PARAM_REPLACE = "Text to replace the search text with";
#include <sstream>
#include <filesystem>

namespace fs = std::filesystem;

// Resolve relative paths to absolute to avoid CWD-dependent failures
static std::string resolveEditPath(const std::string& path) {
    try {
        fs::path p(path);
        return fs::absolute(p).lexically_normal().string();
    } catch (...) {
        return path;
    }
}

// extractStringArg is now in json_utils.h

void registerEditFileTool(ToolRegistry& registry) {
    ToolDefinition def;
    def.name = "edit_file";
    def.description = TOOL_EDIT_FILE_DESC;
    def.parameters = {
        {"path", "string", TOOL_PARAM_EDIT_PATH, true},
        {"search", "string", TOOL_PARAM_SEARCH, true},
        {"replace", "string", TOOL_PARAM_REPLACE, true}
    };

    registry.registerTool(def, [](const std::string& args) -> std::string {
        std::string rawPath = extractStringArg(args, "path");
        std::string searchText = extractStringArg(args, "search");
        std::string replaceText = extractStringArg(args, "replace");

        if (rawPath.empty()) return "Error: Missing 'path' argument";
        if (searchText.empty()) return "Error: Missing 'search' argument";

        // Resolve relative paths to absolute to avoid CWD-dependent failures
        std::string path = resolveEditPath(rawPath);

        // Read the file
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs) return "Error: Cannot open file: " + path;
        std::string content((std::istreambuf_iterator<char>(ifs)), {});

        // -- CRLF-normalized matching -------------------------------
        // Strip \r from both content and searchText for matching,
        // then map the match position back to the original content.
        auto stripCR = [](const std::string& s) -> std::string {
            std::string out;
            out.reserve(s.size());
            for (char c : s) if (c != '\r') out += c;
            return out;
        };

        std::string contentLF = stripCR(content);
        std::string searchLF = stripCR(searchText);

        auto lfPos = contentLF.find(searchLF);
        if (lfPos == std::string::npos) {
            return "Error: Search text not found in " + path +
                   ". Make sure the match is exact (including indentation and newlines).";
        }

        // Map LF-normalized position back to original content
        size_t origPos = 0;
        size_t nonCR = 0;
        while (nonCR < lfPos && origPos < content.size()) {
            if (content[origPos] != '\r') nonCR++;
            origPos++;
        }
        if (origPos >= content.size()) {
            return "Error: Search text not found in " + path +
                   ". Make sure the match is exact (including indentation and newlines).";
        }

        // Compute actual length of the matched region in original content
        size_t actualSearchLen = 0;
        size_t matchedNonCR = 0;
        while (matchedNonCR < searchLF.size() && origPos + actualSearchLen < content.size()) {
            if (content[origPos + actualSearchLen] != '\r') matchedNonCR++;
            actualSearchLen++;
        }

        content.replace(origPos, actualSearchLen, replaceText);

        // Write back
        std::ofstream ofs(path, std::ios::binary);
        if (!ofs) return "Error: Cannot write to: " + path;
        ofs.write(content.data(), content.size());

        return "Edited " + path + ": replaced " + std::to_string(actualSearchLen) +
               " chars with " + std::to_string(replaceText.size()) + " chars.";
    });
}
