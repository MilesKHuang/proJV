#include "file_tool.h"
#include "registry.h"
#include "json_utils.h"
#include <fstream>

// -- Tool description strings (moved from prompts.h) --------------------
static constexpr const char* TOOL_READ_FILE_DESC =
    "Read a file's contents. Use for inspecting source code, configs, and text files.";
static constexpr const char* TOOL_WRITE_FILE_DESC =
    "Write a small amount of content to a file. Creates parent directories if needed. "
    "Maximum content size is 8KB. For anything larger, use exec_shell with stdin.\n"
    "Write via exec_shell+stdin for large files:\n"
    "  exec_shell(command=\"powershell -Command \\\"$input | Set-Content -Path 'file.cpp' -Encoding UTF8\\\"\", stdin=\"<content>\")";
static constexpr const char* TOOL_GREP_FILES_DESC =
    "Search for a pattern in workspace files. Returns matching lines with context.";

// MAX_WRITE_CONTENT_SIZE — write_file size limit
static constexpr size_t MAX_WRITE_CONTENT_SIZE = 8 * 1024;

// Workspace violation constants
static constexpr const char* WS_VIOLATION_PREFIX_INTERNAL = "__WORKSPACE_VIOLATION__:";
static constexpr const char* WS_VIOLATION_VALIDATE_ERROR = " Path '";
static constexpr const char* WS_VIOLATION_VALIDATE_MIDDLE =
    "' is outside the workspace. Only files inside the workspace can be accessed. "
    "Use /workspace to check current workspace, or set workspace_path in config.toml.";

#include <sstream>
#include <filesystem>
#include <regex>
#include <algorithm>

namespace fs = std::filesystem;

// -- Path resolution -----------------------------------------------
// Resolve a path against the workspace: relative paths become absolute
// under the workspace; absolute paths are normalized. Never resolves
// against CWD — CWD is the exe dir, not the workspace.
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
        // If lexically_normal throws (e.g. on malformed paths), return path as-is
        return path;
    }
}

// -- Path validation -----------------------------------------------
static bool isPathInWorkspace(const std::string& resolvedPath, const std::string& workspace) {
    if (workspace.empty()) return true;
    try {
        fs::path base = fs::absolute(workspace).lexically_normal();
        fs::path target(resolvedPath);

        std::string tStr = target.string();
        std::string bStr = base.string();
        std::transform(tStr.begin(), tStr.end(), tStr.begin(), ::tolower);
        std::transform(bStr.begin(), bStr.end(), bStr.begin(), ::tolower);

        // Append separator to base to prevent false match (e.g. D:\ws vs D:\ws2)
        if (!bStr.empty() && bStr.back() != '\\' && bStr.back() != '/')
            bStr += '\\';

        return tStr.find(bStr) == 0;
    } catch (...) {
        return false;
    }
}

// Validate a file path and return the resolved absolute path via outResolved.
// Returns empty string on success, or an error message on violation.
static std::string validateFilePath(const std::string& rawPath, const std::string& workspace,
                                     std::string& outResolved) {
    if (rawPath.empty()) return "Error: Empty path";
    outResolved = resolveWorkspacePath(rawPath, workspace);
    if (!workspace.empty() && !isPathInWorkspace(outResolved, workspace)) {
        // Secondary check: try weakly_canonical for paths that might exist
        // but failed the lexical check (e.g. due to symlinks, junctions,
        // or filesystem quirks). This reduces false positives while still
        // catching genuine workspace escapes.
        try {
            std::error_code ec;
            fs::path canonical = fs::weakly_canonical(rawPath, ec);
            if (!ec && canonical != fs::path(outResolved) && isPathInWorkspace(canonical.string(), workspace)) {
                // False alarm — canonical form is within workspace; update resolved
                outResolved = canonical.string();
                return "";
            }
        } catch (...) {
            // weakly_canonical can throw on some older MSVC; ignore
        }

        return std::string(WS_VIOLATION_PREFIX_INTERNAL) + WS_VIOLATION_VALIDATE_ERROR + rawPath +
               WS_VIOLATION_VALIDATE_MIDDLE;
    }
    return "";
}

static std::string readFileContent(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs) return "Error: Cannot open file: " + path;
    auto size = ifs.tellg();
    if (size > 100 * 1024) return "Error: File too large (>100KB): " + path;
    ifs.seekg(0);
    std::string content((std::istreambuf_iterator<char>(ifs)), {});
    return content;
}

// -- Friendly error: search workspace for same-named files ------------
// When a read_file fails because the path is wrong, this searches the
// workspace for files with the same filename and suggests correct paths.
// This is the "safety net" — even without project context injection,
// the LLM gets immediate feedback about where the file actually lives.
static std::string findSimilarFiles(const std::string& failedPath,
                                     const std::string& workspace) {
    namespace fs = std::filesystem;
    std::error_code ec;

    // Extract just the filename from the failed path
    fs::path fp(failedPath);
    std::string targetName = fp.filename().string();
    if (targetName.empty()) return "";

    // Determine search root
    fs::path root;
    if (!workspace.empty()) {
        root = fs::absolute(workspace, ec);
        if (ec) return "";
    } else {
        root = fs::current_path(ec);
        if (ec) return "";
    }

    std::ostringstream out;
    int found = 0;
    const int MAX_RESULTS = 6;

    // Directories to skip during search
    static const std::vector<std::string> skipDirs = {
        ".git", ".vs", ".vscode", "build", "Debug", "Release",
        "x64", "x86", "bin", "bin64", "obj", "out", "target",
        "node_modules", "packages", "vendor", "external",
        "__pycache__", ".pytest_cache"
    };

    try {
        for (auto it = fs::recursive_directory_iterator(root,
                fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); ) {
            if (ec) { ec.clear(); ++it; continue; }
            if (found >= MAX_RESULTS) break;

            const auto& entry = *it;
            if (entry.is_regular_file(ec) && !ec) {
                if (entry.path().filename().string() == targetName) {
                    // Compute relative path from workspace root
                    std::string relPath = fs::relative(entry.path(), root, ec).string();
                    if (ec) { ec.clear(); relPath = entry.path().string(); }
                    // Normalize backslashes to forward slashes
                    std::replace(relPath.begin(), relPath.end(), '\\', '/');
                    out << "  - " << relPath << "\n";
                    ++found;
                }
            }

            // Skip unwanted directories to speed up search
            if (entry.is_directory(ec) && !ec) {
                std::string dirName = entry.path().filename().string();
                bool skip = (dirName.size() > 0 && dirName[0] == '.');
                if (!skip) {
                    for (const auto& d : skipDirs) {
                        if (dirName == d) { skip = true; break; }
                    }
                }
                if (skip) {
                    it.disable_recursion_pending();
                }
            }
            ++it;
        }
    } catch (...) {
        // Best-effort: return what we found so far
    }

    if (found == 0) return "";
    if (found >= MAX_RESULTS) out << "  ... (more matches truncated)\n";
    return out.str();
}

void registerFileTools(ToolRegistry& registry, const std::string& workspacePath) {
    // read_file
    {
        ToolDefinition def;
        def.name = "read_file";
        def.description = TOOL_READ_FILE_DESC;
        def.parameters = {
            {"path", "string", "Path to the file to read", true}
        };
        registry.registerTool(def, [workspacePath](const std::string& args) -> std::string {
            std::string path = extractStringArg(args, "path");
            if (path.empty()) return "Error: Missing required 'path' argument for read_file. Provide the file path as a JSON string.";
            std::string resolved;
            std::string err = validateFilePath(path, workspacePath, resolved);
            if (!err.empty()) return err;
            std::string content = readFileContent(resolved);
            // -- Friendly error: if file not found, suggest correct paths --
            if (content.find("Error: Cannot open file:") == 0) {
                std::string suggestions = findSimilarFiles(resolved, workspacePath);
                if (!suggestions.empty()) {
                    content += "\n\nDid you mean one of these?\n" + suggestions
                             + "\nPlease use the correct path above to read the file.";
                }
            }
            return content;
        });
    }

    // write_file
    {
        ToolDefinition def;
        def.name = "write_file";
        def.description = TOOL_WRITE_FILE_DESC;
        def.parameters = {
            {"path", "string", "Path to the file", true},
            {"content", "string", "Content to write or append", true},
            {"mode", "string", "Write mode: 'write' (overwrite, default) or 'append' (add to existing)", false}
        };
        registry.registerTool(def, [workspacePath](const std::string& args) -> std::string {
            std::string path = extractStringArg(args, "path");
            std::string content = extractStringArg(args, "content");

            // Validate required 'path' argument with clear guidance
            if (path.empty()) {
                return "Error: Missing required 'path' argument.\n"
                       "write_file requires two arguments:\n"
                       "  - path (string, required): File path to write to\n"
                       "  - content (string, required): Content to write\n"
                       "Example: {\"path\": \"/path/to/file.cpp\", \"content\": \"#include...\"}\n"
                       "Please retry with both arguments provided.";
            }

            // Validate required 'content' argument exists
            if (content.empty()) {
                bool keyExists = false;
                try {
                    auto j = nlohmann::json::parse(args);
                    keyExists = j.contains("content");
                } catch (...) {}
                if (!keyExists) {
                    return "Error: Missing required 'content' argument.\n"
                           "write_file requires both 'path' and 'content' arguments.\n"
                           "Please retry with both arguments provided.";
                }
            }

            // Guard against oversized content that would risk JSON corruption
            if (content.size() > MAX_WRITE_CONTENT_SIZE) {
                size_t chunkSize = MAX_WRITE_CONTENT_SIZE;
                size_t totalSize = content.size();
                size_t numChunks = (totalSize + chunkSize - 1) / chunkSize;
                std::string suggestion =
                    "Error: Content too large (" + std::to_string(totalSize) + " bytes). "
                    "write_file limit is " + std::to_string(MAX_WRITE_CONTENT_SIZE) + " bytes.\n"
                    "\n"
                    "MANDATORY: Split into " + std::to_string(numChunks)
                    + " write_file calls with mode=\"append\".\n"
                    "Each chunk must be under " + std::to_string(MAX_WRITE_CONTENT_SIZE) + " bytes.\n"
                    "DO NOT use exec_shell or any other method for writing files over "
                    + std::to_string(MAX_WRITE_CONTENT_SIZE) + " bytes.\n"
                    "\n"
                    "Template (chunk 1 overwrites, chunks 2..N append):\n"
                    "  1. write_file(path=\"" + path + "\", content=\"<chunk1>\", mode=\"write\")\n";
                for (size_t i = 1; i < numChunks && i < 5; ++i) {
                    suggestion += "  " + std::to_string(i + 1)
                        + ". write_file(path=\"" + path + "\", content=\"<chunk"
                        + std::to_string(i + 1) + ">\", mode=\"append\")\n";
                }
                if (numChunks > 5) {
                    suggestion += "  ... (" + std::to_string(numChunks - 5) + " more chunks)\n";
                }
                return suggestion;
            }

            std::string resolved;
            std::string err = validateFilePath(path, workspacePath, resolved);
            if (!err.empty()) return err;

            try {
                // Determine write mode
                std::string mode = extractStringArg(args, "mode", "write");
                bool appendMode = (mode == "append");

                auto parent = fs::path(resolved).parent_path();
                if (!parent.empty())
                    fs::create_directories(parent);

                std::ios::openmode openFlags = std::ios::out;
                if (appendMode) openFlags |= std::ios::app;

                std::ofstream ofs(resolved, openFlags);
                if (!ofs) return "Error: Cannot write to: " + resolved;
                ofs << content;

                if (appendMode) {
                    return "Appended " + std::to_string(content.size()) + " bytes to " + resolved;
                }
                return "Written " + std::to_string(content.size()) + " bytes to " + resolved;
            } catch (const std::exception& e) {
                return "Error: " + std::string(e.what());
            }
        });
    }

    // grep_files
    {
        ToolDefinition def;
        def.name = "grep_files";
        def.description = TOOL_GREP_FILES_DESC;
        def.parameters = {
            {"pattern", "string", "Regular expression pattern to search for", true},
            {"path", "string", "Directory to search (optional)", false}
        };
        registry.registerTool(def, [workspacePath](const std::string& args) -> std::string {
            std::string pattern = extractStringArg(args, "pattern");
            std::string path = extractStringArg(args, "path");
            if (pattern.empty()) return "Error: Missing 'pattern' argument";
            if (path.empty()) path = workspacePath.empty() ? "." : workspacePath;
            std::string resolved;
            std::string err = validateFilePath(path, workspacePath, resolved);
            if (!err.empty()) return err;
            try {
                std::regex re(pattern, std::regex::ECMAScript | std::regex::icase);
                std::ostringstream result;
                int count = 0;
                for (const auto& entry : fs::recursive_directory_iterator(resolved,
                    fs::directory_options::skip_permission_denied))
                {
                    if (!entry.is_regular_file()) continue;
                    auto ext = entry.path().extension().string();
                    if (ext == ".exe" || ext == ".dll" || ext == ".obj" || ext == ".pdb") continue;

                    std::error_code ec;
                    auto fsize = entry.file_size(ec);
                    if (ec || fsize > 512 * 1024) continue;

                    std::string fpath = entry.path().string();
                    std::string ignored;
                    std::string ferr = validateFilePath(fpath, workspacePath, ignored);
                    if (!ferr.empty()) continue;

                    std::ifstream ifs(entry.path());
                    std::string line;
                    int lineNum = 0;
                    while (std::getline(ifs, line)) {
                        ++lineNum;
                        if (std::regex_search(line, re)) {
                            result << entry.path().string() << ":" << lineNum << ": " << line << "\n";
                            ++count;
                            if (count >= 100) break;
                        }
                    }
                    if (count >= 100) break;
                }
                if (count == 0) return "No matches found for pattern: " + pattern;
                return result.str();
            } catch (const std::exception& e) {
                return "Error: " + std::string(e.what());
            }
        });
    }
}