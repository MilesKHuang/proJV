#include "file_tool.h"
#include "registry.h"
#include "json_utils.h"
#include <fstream>
#include <cstdlib>
#include <functional>
#include <vector>

// -- Tool description strings (moved from prompts.h) --------------------
static constexpr const char* TOOL_READ_FILE_DESC =
    "Read a file's contents. Whole-file reads are capped at 100KB. For larger files, "
    "pass start_line/end_line (1-based, inclusive) to read one slice at a time; the "
    "reply starts with '[lines A-B of T]' and ends with the next start_line when cut.";
static constexpr const char* TOOL_LIST_DIR_DESC =
    "List a directory as an indented tree (directories end with '/'). Start broad "
    "(depth=2 on the workspace root) to see the layout, then deepen on the "
    "subdirectories that matter. Use exclude to skip noise by name. Build artifacts "
    "and VCS directories are always skipped.";
static constexpr const char* TOOL_WRITE_FILE_DESC =
    "Write content to a file. Creates parent directories if needed. Maximum "
    "content size is 8KB per call. For anything larger, split it into chunks: "
    "the first call with mode=\"write\", the rest with mode=\"append\". "
    "Prefer edit_file for changing an existing file.";
static constexpr const char* TOOL_GREP_FILES_DESC =
    "Search workspace files for a regular expression (case-insensitive). Returns "
    "matching lines only, no context, as 'path:line: text'. Stops at max_results "
    "(default 100, max 500); when it stops early the reply ends with "
    "'[truncated at N results]'. Recommended: grep to locate line numbers, then "
    "read_file with start_line/end_line for the surrounding code.";

// MAX_WRITE_CONTENT_SIZE -- write_file size limit
static constexpr size_t MAX_WRITE_CONTENT_SIZE = 8 * 1024;

// Workspace violation constants
static constexpr const char* WS_VIOLATION_PREFIX_INTERNAL = "__WORKSPACE_VIOLATION__:";
static constexpr const char* WS_VIOLATION_VALIDATE_ERROR = " Path '";
static constexpr const char* WS_VIOLATION_VALIDATE_MIDDLE =
    "' is outside the workspace. Only files inside the workspace can be accessed. "
    "Use /workspace to check the current workspace, which is the directory proJV was "
    "launched from.";

#include <sstream>
#include <filesystem>
#include <regex>
#include <algorithm>

namespace fs = std::filesystem;

// -- Directories to skip during recursive traversal -----------------
// Shared by findSimilarFiles and grep_files to avoid crawling huge
// build-artifact / VCS / dependency directories that would freeze the UI.
static const std::vector<std::string> kSkipDirs = {
    ".git",     ".vs",    ".vscode",  "build",       "Debug",        "Release", "x64",
    "x86",      "bin",    "bin64",    "obj",         "out",          "target",  "node_modules",
    "packages", "vendor", "external", "__pycache__", ".pytest_cache"};

// -- Path resolution -----------------------------------------------
// Resolve a path against the workspace: relative paths become absolute
// under the workspace; absolute paths are normalized. Never resolves
// against CWD -- CWD is the exe dir, not the workspace.
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
// True when `resolvedPath` is the workspace root or lives under it.
// Component-wise comparison, not string prefix: that keeps it separator
// agnostic (Linux has no '\\') and makes D:\ws vs D:\ws2 impossible to confuse.
static bool isPathInWorkspace(const std::string& resolvedPath, const std::string& workspace) {
    if (workspace.empty()) return true;
    try {
        const fs::path base = fs::absolute(workspace).lexically_normal();
        const fs::path target = fs::path(resolvedPath).lexically_normal();
        auto bit = base.begin();
        auto tit = target.begin();
        for (; bit != base.end(); ++bit, ++tit) {
            if (tit == target.end()) return false; // target is above the workspace
            std::string a = bit->string();
            std::string b = tit->string();
#ifdef _WIN32
            // Windows paths are case-insensitive.
            auto lower = [](std::string& s) {
                for (auto& c : s)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            };
            lower(a);
            lower(b);
#endif
            if (a != b) return false;
        }
        return true;
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
            if (!ec && canonical != fs::path(outResolved) &&
                isPathInWorkspace(canonical.string(), workspace)) {
                // False alarm -- canonical form is within workspace; update resolved
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

// Public wrapper: other file-touching tools (format_file) share this guard.
std::string validateToolPath(const std::string& rawPath, const std::string& workspace,
                             std::string& outResolved) {
    return validateFilePath(rawPath, workspace, outResolved);
}

// -- file_requests spec: "path", "path#L120", "path#L120-L480" ----------
bool parseFileRequestSpec(const std::string& spec, std::string& outPath, int& startLine,
                          int& endLine) {
    outPath.clear();
    startLine = 0;
    endLine = 0;
    if (spec.empty()) return false;

    // The range suffix is optional; a '#' that is not followed by L<digits>
    // belongs to the path itself.
    size_t hash = spec.rfind('#');
    if (hash != std::string::npos && hash + 2 < spec.size()) {
        std::string tail = spec.substr(hash + 1);
        auto isL = [](char c) { return c == 'L' || c == 'l'; };
        size_t i = 0;
        if (isL(tail[0])) ++i;
        size_t digitStart = i;
        while (i < tail.size() && tail[i] >= '0' && tail[i] <= '9')
            ++i;
        bool firstNum = (i > digitStart);
        int a = firstNum ? std::atoi(tail.substr(digitStart, i - digitStart).c_str()) : 0;
        int b = 0;
        bool okRange = firstNum;
        if (firstNum && i < tail.size() && tail[i] == '-') {
            ++i;
            size_t s2 = i;
            if (i < tail.size() && isL(tail[i])) ++i;
            size_t d2 = i;
            while (i < tail.size() && tail[i] >= '0' && tail[i] <= '9')
                ++i;
            if (i > d2)
                b = std::atoi(tail.substr(d2, i - d2).c_str());
            else
                okRange = false;
            (void)s2;
        }
        if (okRange && i == tail.size() && a > 0) {
            outPath = spec.substr(0, hash);
            startLine = a;
            endLine = b > 0 ? b : 0;
            return true;
        }
    }
    outPath = spec;
    return true;
}

// -- read_file: whole-file read -----------------------------------------
static std::string readFileContent(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs) return "Error: Cannot open file: " + path;
    auto size = ifs.tellg();
    if (size > 100 * 1024) return "Error: File too large (>100KB): " + path;
    ifs.seekg(0);
    std::string content((std::istreambuf_iterator<char>(ifs)), {});
    return content;
}

// -- read_file: line-range read -----------------------------------------
static constexpr size_t kRangeMaxFileSize = 2 * 1024 * 1024; // 2MB scanned
static constexpr size_t kRangeMaxBytes = 100 * 1024;         // 100KB returned
static constexpr int kRangeMaxLines = 1000;

// Accepts a JSON number or a numeric string (models send both).
static int intArgOrZero(const std::string& args, const char* key) {
    try {
        auto j = nlohmann::json::parse(args);
        if (!j.contains(key)) return 0;
        const auto& v = j[key];
        if (v.is_number_integer()) return v.get<int>();
        if (v.is_number()) return static_cast<int>(v.get<double>());
        if (v.is_string()) {
            const std::string s = v.get<std::string>();
            try {
                return std::stoi(s);
            } catch (...) {
                return 0;
            }
        }
    } catch (...) {
    }
    return 0;
}

static std::string readFileRange(const std::string& path, int startLine, int endLine) {
    std::error_code ec;
    auto fsize = fs::file_size(path, ec);
    if (!ec && fsize > kRangeMaxFileSize)
        return "Error: File too large for a range read (>2MB): " + path;

    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return "Error: Cannot open file: " + path;

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(ifs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    const int total = static_cast<int>(lines.size());

    int s = startLine > 0 ? startLine : 1;
    int e = endLine > 0 ? endLine : total;
    if (s < 1) return "Error: start_line must be >= 1 (got " + std::to_string(startLine) + ")";
    if (endLine > 0 && endLine < s)
        return "Error: end_line (" + std::to_string(endLine) + ") must be >= start_line (" +
               std::to_string(s) + ")";
    if (s > total)
        return "Error: start_line " + std::to_string(s) + " is beyond the end of the file (" +
               std::to_string(total) + " lines)";
    if (e > total) e = total;

    std::string out = "[lines " + std::to_string(s) + "-" + std::to_string(e) + " of " +
                      std::to_string(total) + "]\n";
    int last = s - 1;
    for (int n = s; n <= e; ++n) {
        std::string body = std::to_string(n) + ": " + lines[static_cast<size_t>(n - 1)] + "\n";
        if (out.size() + body.size() > kRangeMaxBytes) {
            if (n == s) {
                // Never return an empty range because of one very long line.
                size_t room = kRangeMaxBytes - out.size();
                if (room > 16) {
                    body.resize(room - 1);
                    body += "\n";
                    out += body;
                    last = n;
                }
            }
            break;
        }
        out += body;
        last = n;
        if ((n - s + 1) >= kRangeMaxLines) break;
    }
    if (last < e)
        out += "\n[truncated, continue with start_line=" + std::to_string(last + 1) + "]\n";
    return out;
}

// -- Friendly error: search workspace for same-named files ------------
// When a read_file fails because the path is wrong, this searches the
// workspace for files with the same filename and suggests correct paths.
// This is the "safety net" -- even without project context injection,
// the LLM gets immediate feedback about where the file actually lives.
static std::string findSimilarFiles(const std::string& failedPath, const std::string& workspace) {
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

    // Use file-level kSkipDirs defined above
    try {
        for (auto it = fs::recursive_directory_iterator(
                 root, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator();) {
            if (ec) {
                ec.clear();
                ++it;
                continue;
            }
            if (found >= MAX_RESULTS) break;

            const auto& entry = *it;
            if (entry.is_regular_file(ec) && !ec) {
                if (entry.path().filename().string() == targetName) {
                    // Compute relative path from workspace root
                    std::string relPath = fs::relative(entry.path(), root, ec).string();
                    if (ec) {
                        ec.clear();
                        relPath = entry.path().string();
                    }
                    // Normalize backslashes to forward slashes
                    std::replace(relPath.begin(), relPath.end(), '\\', '/');
                    out << "  - " << relPath << "\n";
                    ++found;
                }
            }

            // Skip unwanted directories to speed up search
            if (entry.is_directory(ec) && !ec) {
                std::string dirName = entry.path().filename().string();
                if (isSkippedDirName(dirName)) {
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

// -- list_dir -------------------------------------------------------------
static constexpr int kListDefaultDepth = 2;
static constexpr int kListMaxDepth = 6;
static constexpr int kListMaxEntries = 500;

bool isSkippedDirName(const std::string& name) {
    if (name.empty()) return true;
    if (name[0] == '.') return true; // .git / .vs / .pytest_cache ...
    for (const auto& d : kSkipDirs)
        if (name == d) return true;
    // Build trees are not always called "build": this repository alone has
    // build_linux / build_strict / cmake-build-*, and an unscoped grep that
    // descended into them truncated at the result cap (observed in a real run).
    if (name.rfind("build_", 0) == 0) return true;
    if (name.rfind("cmake-build", 0) == 0) return true;
    return false;
}

static size_t countChildEntries(const fs::path& dir) {
    std::error_code ec;
    size_t n = 0;
    for (auto it = fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) {
            ec.clear();
            break;
        }
        ++n;
    }
    return n;
}

// Indented tree, directories first and then files, both name-sorted.
// Stops at `depth` with an entry count so the caller can decide to deepen.
static std::string listDirTree(const fs::path& root, int depth, const std::regex* exclude) {
    std::ostringstream out;
    int emitted = 0;
    bool truncated = false;

    std::function<void(const fs::path&, int)> walk = [&](const fs::path& dir, int level) {
        if (truncated) return;
        std::error_code ec;
        std::vector<fs::directory_entry> dirs, files;
        for (auto it =
                 fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
             it != fs::directory_iterator(); it.increment(ec)) {
            if (ec) {
                ec.clear();
                break;
            }
            const fs::directory_entry& e = *it;
            const std::string name = e.path().filename().string();
            std::error_code ec2;
            const bool isDir = e.is_directory(ec2);
            if (!isDir && !e.is_regular_file(ec2)) continue;
            if (isDir) {
                if (isSkippedDirName(name)) continue;
            } else if (name.empty())
                continue;
            if (exclude && std::regex_search(name, *exclude)) continue;
            (isDir ? dirs : files).push_back(e);
        }
        auto byName = [](const fs::directory_entry& a, const fs::directory_entry& b) {
            return a.path().filename().string() < b.path().filename().string();
        };
        std::sort(dirs.begin(), dirs.end(), byName);
        std::sort(files.begin(), files.end(), byName);

        const std::string pad(static_cast<size_t>(level) * 2, ' ');
        for (const auto& e : dirs) {
            if (emitted >= kListMaxEntries) {
                truncated = true;
                return;
            }
            ++emitted;
            out << pad << e.path().filename().string() << "/";
            if (level + 1 >= depth) {
                out << " (" << countChildEntries(e.path()) << " entries, not expanded)\n";
            } else {
                out << "\n";
                walk(e.path(), level + 1);
            }
        }
        for (const auto& e : files) {
            if (emitted >= kListMaxEntries) {
                truncated = true;
                return;
            }
            ++emitted;
            out << pad << e.path().filename().string() << "\n";
        }
    };

    walk(root, 0);
    std::string body = out.str();
    if (body.empty()) body = "(empty)\n";
    if (truncated)
        body += "\n[truncated at " + std::to_string(kListMaxEntries) +
                " entries - lower depth or narrow path]\n";
    return body;
}

void registerFileTools(ToolRegistry& registry, const std::string& workspacePath) {
    // read_file
    {
        ToolDefinition def;
        def.name = "read_file";
        def.description = TOOL_READ_FILE_DESC;
        def.parameters = {
            {"path", "string", "Path to the file to read", true},
            {"start_line", "number",
             "Optional, 1-based first line of the slice (inclusive). Omit to start at line 1.",
             false},
            {"end_line", "number",
             "Optional, 1-based last line of the slice (inclusive). Omit to read to the end.",
             false}};
        registry.registerTool(def, [workspacePath](const std::string& args) -> std::string {
            std::string path = extractStringArg(args, "path");
            if (path.empty())
                return "Error: Missing required 'path' argument for read_file. Provide the file "
                       "path as a JSON string.";
            std::string resolved;
            std::string err = validateFilePath(path, workspacePath, resolved);
            if (!err.empty()) return err;
            const int startLine = intArgOrZero(args, "start_line");
            const int endLine = intArgOrZero(args, "end_line");
            std::string content = (startLine <= 0 && endLine <= 0)
                                      ? readFileContent(resolved) // legacy whole-file read
                                      : readFileRange(resolved, startLine, endLine);
            // -- Friendly error: if file not found, suggest correct paths --
            if (content.find("Error: Cannot open file:") == 0) {
                std::string suggestions = findSimilarFiles(resolved, workspacePath);
                if (!suggestions.empty()) {
                    content += "\n\nDid you mean one of these?\n" + suggestions +
                               "\nPlease use the correct path above to read the file.";
                }
            }
            return content;
        });
    }

    // list_dir
    {
        ToolDefinition def;
        def.name = "list_dir";
        def.description = TOOL_LIST_DIR_DESC;
        def.parameters = {
            {"path", "string", "Directory to list (optional; default = workspace root)", false},
            {"depth", "number", "Levels to expand, 1..6 (default 2)", false},
            {"exclude", "string", "Optional regex; entries whose name matches are hidden", false}};
        registry.registerTool(def, [workspacePath](const std::string& args) -> std::string {
            std::string path = extractStringArg(args, "path");
            if (path.empty()) path = workspacePath.empty() ? std::string(".") : workspacePath;
            std::string resolved;
            std::string err = validateFilePath(path, workspacePath, resolved);
            if (!err.empty()) return err;

            std::error_code ec;
            if (!fs::is_directory(resolved, ec)) return "Error: Not a directory: " + resolved;

            int depth = intArgOrZero(args, "depth");
            bool depthCapped = false;
            if (depth <= 0) depth = kListDefaultDepth;
            if (depth > kListMaxDepth) {
                depth = kListMaxDepth;
                depthCapped = true;
            }

            const std::string exclude = extractStringArg(args, "exclude");
            std::regex excludeRe;
            bool hasExclude = false;
            if (!exclude.empty()) {
                try {
                    excludeRe = std::regex(exclude, std::regex::ECMAScript | std::regex::icase);
                    hasExclude = true;
                } catch (const std::exception& e) {
                    return "Error: invalid 'exclude' regex (" + std::string(e.what()) + ")";
                }
            }

            std::string header = "[list_dir] " + resolved + " (depth=" + std::to_string(depth) +
                                 (depthCapped ? ", capped at 6" : "") + ")\n";
            return header +
                   listDirTree(fs::path(resolved), depth, hasExclude ? &excludeRe : nullptr);
        });
    }

    // write_file
    {
        ToolDefinition def;
        def.name = "write_file";
        def.description = TOOL_WRITE_FILE_DESC;
        def.parameters = {{"path", "string", "Path to the file", true},
                          {"content", "string", "Content to write or append", true},
                          {"mode", "string",
                           "Write mode: 'write' (overwrite, default) or 'append' (add to existing)",
                           false}};
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
                } catch (...) {
                }
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
                std::string suggestion = "Error: Content too large (" + std::to_string(totalSize) +
                                         " bytes). "
                                         "write_file limit is " +
                                         std::to_string(MAX_WRITE_CONTENT_SIZE) +
                                         " bytes.\n"
                                         "\n"
                                         "MANDATORY: Split into " +
                                         std::to_string(numChunks) +
                                         " write_file calls with mode=\"append\".\n"
                                         "Each chunk must be under " +
                                         std::to_string(MAX_WRITE_CONTENT_SIZE) +
                                         " bytes.\n"
                                         "Do not try any other method for writing files over " +
                                         std::to_string(MAX_WRITE_CONTENT_SIZE) +
                                         " bytes.\n"
                                         "\n"
                                         "Template (chunk 1 overwrites, chunks 2..N append):\n"
                                         "  1. write_file(path=\"" +
                                         path + "\", content=\"<chunk1>\", mode=\"write\")\n";
                for (size_t i = 1; i < numChunks && i < 5; ++i) {
                    suggestion += "  " + std::to_string(i + 1) + ". write_file(path=\"" + path +
                                  "\", content=\"<chunk" + std::to_string(i + 1) +
                                  ">\", mode=\"append\")\n";
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
                if (!parent.empty()) fs::create_directories(parent);

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
            {"path", "string", "Directory to search (optional)", false},
            {"max_results", "number", "Stop after this many hits: default 100, max 500", false}};
        registry.registerTool(def, [workspacePath](const std::string& args) -> std::string {
            std::string pattern = extractStringArg(args, "pattern");
            std::string path = extractStringArg(args, "path");
            if (pattern.empty()) return "Error: Missing 'pattern' argument";
            if (path.empty()) path = workspacePath.empty() ? "." : workspacePath;
            int maxResults = intArgOrZero(args, "max_results");
            if (maxResults <= 0) maxResults = 100;
            if (maxResults > 500) maxResults = 500;
            std::string resolved;
            std::string err = validateFilePath(path, workspacePath, resolved);
            if (!err.empty()) return err;
            try {
                std::regex re(pattern, std::regex::ECMAScript | std::regex::icase);
                std::ostringstream result;
                int count = 0;
                int fileCount = 0;
                bool hitLimit = false;
                static constexpr int kMaxFilesToScan = 5000; // hard cap to prevent UI freeze
                std::error_code ec;

                // A plain file path means "search this one file". Passing it to
                // recursive_directory_iterator silently yields no matches, which
                // reads as "no callers" and corrupts dead-code conclusions.
                if (fs::is_regular_file(resolved, ec)) {
                    std::ifstream ifs(resolved);
                    std::string line;
                    int lineNum = 0;
                    while (std::getline(ifs, line)) {
                        ++lineNum;
                        if (!std::regex_search(line, re)) continue;
                        result << resolved << ":" << lineNum << ": " << line << "\n";
                        if (++count >= maxResults) {
                            hitLimit = true;
                            break;
                        }
                    }
                    if (count == 0) return "No matches found for pattern: " + pattern;
                    std::string out = result.str();
                    if (hitLimit)
                        out += "\n[truncated at " + std::to_string(count) +
                               " results - narrow the pattern, or raise max_results]\n";
                    return out;
                }

                for (auto it = fs::recursive_directory_iterator(
                         resolved, fs::directory_options::skip_permission_denied, ec);
                     it != fs::recursive_directory_iterator();) {
                    if (ec) {
                        ec.clear();
                        ++it;
                        continue;
                    }

                    const auto& entry = *it;

                    // Skip known junk directories
                    if (entry.is_directory()) {
                        std::string dirName = entry.path().filename().string();
                        if (isSkippedDirName(dirName)) {
                            it.disable_recursion_pending();
                            ++it;
                            continue;
                        }
                    }
                    if (!entry.is_regular_file()) {
                        ++it;
                        continue;
                    }
                    auto ext = entry.path().extension().string();
                    if (ext == ".exe" || ext == ".dll" || ext == ".obj" || ext == ".pdb") {
                        ++it;
                        continue;
                    }

                    std::error_code ec2;
                    auto fsize = entry.file_size(ec2);
                    if (ec2 || fsize > 512 * 1024) {
                        ++it;
                        continue;
                    }

                    std::string fpath = entry.path().string();
                    std::string ignored;
                    std::string ferr = validateFilePath(fpath, workspacePath, ignored);
                    if (!ferr.empty()) {
                        ++it;
                        continue;
                    }

                    std::ifstream ifs(entry.path());
                    std::string line;
                    int lineNum = 0;
                    ++fileCount;
                    while (std::getline(ifs, line)) {
                        ++lineNum;
                        if (std::regex_search(line, re)) {
                            result << entry.path().string() << ":" << lineNum << ": " << line
                                   << "\n";
                            ++count;
                            if (count >= maxResults) break;
                        }
                    }
                    if (count >= maxResults) {
                        hitLimit = true;
                        break;
                    }
                    if (fileCount >= kMaxFilesToScan) {
                        result << "\n[grep_files: reached file scan limit (" << kMaxFilesToScan
                               << "). Results may be incomplete.]\n";
                        break;
                    }
                    ++it;
                }
                if (result.str().empty()) return "No matches found for pattern: " + pattern;
                std::string out = result.str();
                if (hitLimit)
                    out += "\n[truncated at " + std::to_string(count) +
                           " results - narrow the pattern/path, or raise max_results]\n";
                return out;
            } catch (const std::exception& e) {
                return "Error: " + std::string(e.what());
            }
        });
    }
}
