#include "md_file_tool.h"
#include "registry.h"
#include "json_utils.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>

namespace fs = std::filesystem;

// -- Tool description strings --------------------------------------------
static constexpr const char* TOOL_MD_FILE_DESC =
    "Write or edit markdown (.md) design documents. "
    "Actions: write (overwrite/create, max 8KB), edit (SEARCH/REPLACE). "
    "Only .md files are allowed. Use read_file / grep_files to read project code.";

static constexpr size_t MAX_MD_CONTENT_SIZE = 8 * 1024;

// -- Workspace violation constants (same as file_tool.cpp) ---------------
static constexpr const char* WS_VIOLATION_PREFIX_INTERNAL = "__WORKSPACE_VIOLATION__:";
static constexpr const char* WS_VIOLATION_VALIDATE_ERROR = " Path '";
static constexpr const char* WS_VIOLATION_VALIDATE_MIDDLE =
    "' is outside the workspace. Only files inside the workspace can be accessed. "
    "Use /workspace to check current workspace, or set workspace_path in config.toml.";

// -- Path resolution (duplicated from file_tool.cpp for independence) ---
static std::string resolveWorkspacePath(const std::string& path, const std::string& workspace) {
    try {
        fs::path inputPath(path);
        if (inputPath.is_absolute()) {
            return fs::absolute(inputPath).lexically_normal().string();
        }
        if (workspace.empty()) {
            return fs::absolute(inputPath).lexically_normal().string();
        }
        fs::path base = fs::absolute(workspace).lexically_normal();
        return (base / inputPath).lexically_normal().string();
    } catch (...) {
        return path;
    }
}

static bool isPathInWorkspace(const std::string& resolvedPath, const std::string& workspace) {
    if (workspace.empty()) return true;
    try {
        fs::path base = fs::absolute(workspace).lexically_normal();
        fs::path target(resolvedPath);

        std::string tStr = target.string();
        std::string bStr = base.string();
        std::transform(tStr.begin(), tStr.end(), tStr.begin(), ::tolower);
        std::transform(bStr.begin(), bStr.end(), bStr.begin(), ::tolower);

        if (!bStr.empty() && bStr.back() != '\\' && bStr.back() != '/')
            bStr += '\\';

        return tStr.find(bStr) == 0;
    } catch (...) {
        return false;
    }
}

static std::string validateFilePath(const std::string& rawPath, const std::string& workspace,
                                     std::string& outResolved) {
    if (rawPath.empty()) return "Error: Empty path";
    outResolved = resolveWorkspacePath(rawPath, workspace);
    if (!workspace.empty() && !isPathInWorkspace(outResolved, workspace)) {
        try {
            std::error_code ec;
            fs::path canonical = fs::weakly_canonical(rawPath, ec);
            if (!ec && canonical != fs::path(outResolved) && isPathInWorkspace(canonical.string(), workspace)) {
                outResolved = canonical.string();
                return "";
            }
        } catch (...) {}

        return std::string(WS_VIOLATION_PREFIX_INTERNAL) + WS_VIOLATION_VALIDATE_ERROR + rawPath +
               WS_VIOLATION_VALIDATE_MIDDLE;
    }
    return "";
}

// -- Validate .md suffix --------------------------------------------------
static std::string validateMdSuffix(const std::string& path) {
    if (path.size() < 3) return "Error: Path too short, must end with .md";
    std::string lower = path;
    std::transform(lower.end() - 3, lower.end(), lower.end() - 3, ::tolower);
    if (lower.compare(lower.size() - 3, 3, ".md") != 0) {
        return "Error: md_file only supports .md files. Path must end with .md.";
    }
    return "";
}

void registerMdFileTool(ToolRegistry& registry, const std::string& workspacePath) {
    ToolDefinition def;
    def.name = "md_file";
    def.description = TOOL_MD_FILE_DESC;
    def.parameters = {
        {"action", "string", "Action: 'write' (overwrite/create, max 8KB) or 'edit' (SEARCH/REPLACE)", true},
        {"path", "string", "Path to the .md file (must end with .md)", true},
        {"content", "string", "Content to write (required for 'write' action)", false},
        {"search", "string", "Text to search for (required for 'edit' action)", false},
        {"replace", "string", "Replacement text (required for 'edit' action)", false}
    };

    registry.registerTool(def, [workspacePath](const std::string& args) -> std::string {
        std::string action = extractStringArg(args, "action");
        std::string path = extractStringArg(args, "path");

        if (action.empty()) return "Error: Missing required 'action' argument. Must be 'write' or 'edit'.";
        if (path.empty()) return "Error: Missing required 'path' argument.";

        // Validate .md suffix
        std::string suffixErr = validateMdSuffix(path);
        if (!suffixErr.empty()) return suffixErr;

        // Validate path is in workspace
        std::string resolved;
        std::string pathErr = validateFilePath(path, workspacePath, resolved);
        if (!pathErr.empty()) return pathErr;

        if (action == "write") {
            std::string content = extractStringArg(args, "content");
            if (content.empty()) {
                bool keyExists = false;
                try {
                    auto j = nlohmann::json::parse(args);
                    keyExists = j.contains("content");
                } catch (...) {}
                if (!keyExists) {
                    return "Error: Missing required 'content' argument for 'write' action.";
                }
            }

            if (content.size() > MAX_MD_CONTENT_SIZE) {
                return "Error: Content too large (" + std::to_string(content.size())
                     + " bytes). Maximum is " + std::to_string(MAX_MD_CONTENT_SIZE) + " bytes.";
            }

            try {
                auto parent = fs::path(resolved).parent_path();
                if (!parent.empty())
                    fs::create_directories(parent);

                std::ofstream ofs(resolved, std::ios::out);
                if (!ofs) return "Error: Cannot write to: " + resolved;
                ofs << content;
                return "Written " + std::to_string(content.size()) + " bytes to " + resolved;
            } catch (const std::exception& e) {
                return "Error: " + std::string(e.what());
            }
        }
        else if (action == "edit") {
            std::string search = extractStringArg(args, "search");
            std::string replace = extractStringArg(args, "replace");

            if (search.empty()) return "Error: Missing required 'search' argument for 'edit' action.";
            if (replace.empty()) return "Error: Missing required 'replace' argument for 'edit' action.";

            // Read existing file
            std::ifstream ifs(resolved, std::ios::binary | std::ios::ate);
            if (!ifs) return "Error: Cannot open file for editing: " + resolved;
            auto size = ifs.tellg();
            if (size > 200 * 1024) return "Error: File too large for edit (>200KB): " + resolved;
            ifs.seekg(0);
            std::string content((std::istreambuf_iterator<char>(ifs)), {});

            // SEARCH/REPLACE
            size_t pos = content.find(search);
            if (pos == std::string::npos) {
                // Try with normalized line endings
                std::string contentLF = content;
                size_t crPos = 0;
                while ((crPos = contentLF.find("\r\n", crPos)) != std::string::npos) {
                    contentLF.replace(crPos, 2, "\n");
                    crPos += 1;
                }
                std::string searchLF = search;
                crPos = 0;
                while ((crPos = searchLF.find("\r\n", crPos)) != std::string::npos) {
                    searchLF.replace(crPos, 2, "\n");
                    crPos += 1;
                }
                pos = contentLF.find(searchLF);
                if (pos != std::string::npos) {
                    // Map LF positions back to original content offsets.
                    // file has \r\n, search has \n -- we must compute BOTH
                    // the start position AND the actual byte length.
                    size_t origPos = 0;
                    for (size_t i = 0; i < pos; ++i) {
                        if (origPos < content.size() && content[origPos] == '\r')
                            origPos += 2;  // \r\n
                        else
                            origPos += 1;
                    }
                    size_t origEnd = origPos;
                    for (size_t j = 0; j < searchLF.size(); ++j) {
                        if (origEnd < content.size() && content[origEnd] == '\r')
                            origEnd += 2;
                        else
                            origEnd += 1;
                    }
                    size_t origLen = origEnd - origPos;
                    content.replace(origPos, origLen, replace);
                    std::ofstream ofs(resolved, std::ios::out | std::ios::trunc);
                    if (!ofs) return "Error: Cannot write to: " + resolved;
                    ofs << content;
                    return "Edited " + resolved + " (match found after LF normalization)";
                }
                return "Error: Search text not found in file: " + resolved
                     + "\nSearch text: " + (search.size() > 80 ? search.substr(0, 80) + "..." : search);
            }

            // Count occurrences
            size_t count = 0;
            size_t tmp = pos;
            while (tmp != std::string::npos) {
                ++count;
                tmp = content.find(search, tmp + 1);
            }
            if (count > 1) {
                return "Error: Search text found " + std::to_string(count) + " times. "
                     "Make the search text more specific to target a single occurrence.";
            }

            content.replace(pos, search.size(), replace);
            std::ofstream ofs(resolved, std::ios::out | std::ios::trunc);
            if (!ofs) return "Error: Cannot write to: " + resolved;
            ofs << content;
            return "Edited " + resolved;
        }
        else {
            return "Error: Unknown action '" + action + "'. Must be 'write' or 'edit'.";
        }
    });
}
