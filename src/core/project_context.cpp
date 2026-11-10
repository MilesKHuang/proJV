// proJV -- project context builder implementation (extracted from legacy ui/app.cpp).
#include "project_context.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr const char* PROJECT_CONTEXT_PREFIX =
    "[PROJECT CONTEXT] Workspace directory structure -- use these paths with read_file:\n"
    "Workspace: ";
constexpr const char* PROJECT_CONTEXT_SUFFIX = "\n[END PROJECT CONTEXT]";
constexpr const char* PROJECT_EMPTY =
    "[PROJECT CONTEXT] Workspace directory structure -- use these paths with read_file:\n"
    "Workspace: (none)\n  (empty: no top-level directories or files to list. Use list_dir to "
    "look.)"
    "\n[END PROJECT CONTEXT]";

// 600 characters, not 4500. The tree is a hint, not a map: list_dir is one call
// away, and the budget is better spent on AGENTS.md and the conversation.
constexpr size_t kProjectMaxChars = 600;
// Room kept for the "... (truncated ...)" line so the note itself always fits.
constexpr size_t kTruncationReserve = 110;
constexpr size_t kMaxSubDirs = 6;
constexpr size_t kMaxFiles = 8;

// -- AGENTS.md ---------------------------------------------------------------
constexpr const char* AGENTS_MD_PREFIX =
    "[AGENTS.MD] Project instructions from the repository owner. Follow them unless the user "
    "says otherwise.\n";
constexpr const char* AGENTS_MD_SUFFIX = "\n[END AGENTS.MD]";
constexpr const char* AGENTS_EMPTY =
    "[AGENTS.MD] (none: this workspace root has no AGENTS.md.)\n[END AGENTS.MD]";
constexpr size_t kAgentsMdMaxBytes = 8 * 1024;

fs::path workspaceRoot(const std::string& workspacePath, std::error_code& ec) {
    if (workspacePath.empty()) return fs::current_path(ec);
    return fs::absolute(workspacePath, ec);
}

} // namespace

std::string buildProjectContext(const std::string& workspacePath) {
    std::error_code ec;

    fs::path root;
    if (workspacePath.empty()) {
        root = fs::current_path(ec);
        if (ec) return "";
    } else {
        root = fs::absolute(workspacePath, ec);
        if (ec) return "";
    }
    if (!fs::is_directory(root, ec) || ec) return "";

    static const std::vector<std::string> skipDirs = {
        ".git",        ".vs",           ".vscode",      ".idea",    "build",  "Debug",
        "Release",     "x64",           "x86",          "bin",      "bin64",  "obj",
        "out",         "target",        "node_modules", "packages", "vendor", "external",
        "__pycache__", ".pytest_cache", ".mypy_cache",  "nix",      "deploy", "integrations",
        "fuzzing",     "docs",          "examples",     "tests",    "test",   ".github"};

    static const std::vector<std::string> srcExts = {".cpp",
                                                     ".c",
                                                     ".h",
                                                     ".hpp",
                                                     ".rs",
                                                     ".py",
                                                     ".go",
                                                     ".ts",
                                                     ".js",
                                                     ".toml",
                                                     ".json",
                                                     ".yaml",
                                                     ".yml",
                                                     ".cmake",
                                                     ".txt",
                                                     ".md",
                                                     "CMakeLists.txt",
                                                     "Makefile",
                                                     "Cargo.toml",
                                                     "package.json"};

    auto shouldSkip = [](const std::string& name) -> bool {
        if (name.empty() || name[0] == '.') return true;
        for (const auto& d : skipDirs) {
            if (name == d) return true;
        }
        return false;
    };

    auto isSrcFile = [](const std::string& name) -> bool {
        for (const auto& ext : srcExts) {
            if (name.size() >= ext.size() &&
                name.compare(name.size() - ext.size(), ext.size(), ext) == 0)
                return true;
        }
        return false;
    };

    std::vector<fs::directory_entry> topEntries;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        topEntries.push_back(entry);
    }
    std::sort(topEntries.begin(), topEntries.end(),
              [](const fs::directory_entry& a, const fs::directory_entry& b) {
                  const bool aDir = a.is_directory();
                  const bool bDir = b.is_directory();
                  if (aDir != bDir) return aDir > bDir;
                  return a.path().filename().string() < b.path().filename().string();
              });

    std::ostringstream out;
    out << PROJECT_CONTEXT_PREFIX << root.string() << "\n";

    size_t used = out.str().size();
    const size_t budget = kProjectMaxChars > kTruncationReserve
                              ? kProjectMaxChars - kTruncationReserve
                              : kProjectMaxChars;
    size_t listedDirs = 0, filesListed = 0, skippedEntries = 0;
    std::vector<std::string> files;

    for (const auto& entry : topEntries) {
        const std::string name = entry.path().filename().string();
        if (shouldSkip(name)) continue;

        if (entry.is_directory(ec)) {
            if (ec) {
                ec.clear();
                continue;
            }
            // One line per directory: its immediate sub-directories, names only.
            std::vector<std::string> subs;
            for (const auto& sub : fs::directory_iterator(entry.path(), ec)) {
                if (ec) {
                    ec.clear();
                    break;
                }
                if (subs.size() >= kMaxSubDirs) break;
                const std::string subName = sub.path().filename().string();
                if (shouldSkip(subName)) continue;
                if (!sub.is_directory(ec)) continue;
                subs.push_back(subName + "/");
            }
            std::string line = "  " + name + "/";
            if (!subs.empty()) {
                line += ":";
                for (const auto& s : subs)
                    line += " " + s;
            }
            line += "\n";
            if (used + line.size() > budget) {
                ++skippedEntries;
                continue;
            }
            out << line;
            used += line.size();
            ++listedDirs;
        } else if (entry.is_regular_file(ec) && isSrcFile(name)) {
            files.push_back(name);
        }
    }

    // Top-level files share one line: "  CMakeLists.txt  README.md  AGENTS.md".
    std::string fileLine;
    for (const auto& f : files) {
        const std::string candidate = fileLine.empty() ? f : fileLine + "  " + f;
        if (filesListed >= kMaxFiles || used + 3 + candidate.size() > budget) {
            ++skippedEntries;
            continue;
        }
        fileLine = candidate;
        ++filesListed;
    }
    if (!fileLine.empty()) {
        out << "  " << fileLine << "\n";
        used += fileLine.size() + 3;
    }

    if (listedDirs + filesListed == 0 && skippedEntries == 0) {
        out << "  (empty: no top-level directories or files to list. Use list_dir to look.)\n";
    } else if (skippedEntries > 0) {
        // Never let a cut look like the whole picture.
        out << "  ... (truncated at " << kProjectMaxChars << " chars: " << skippedEntries
            << " more entries not listed. Use list_dir for the full tree.)\n";
    }

    out << PROJECT_CONTEXT_SUFFIX;
    return out.str();
}

std::string buildAgentsMdContext(const std::string& workspacePath) {
    std::error_code ec;
    const fs::path root = workspaceRoot(workspacePath, ec);
    if (ec || root.empty()) return "";

    const fs::path file = root / "AGENTS.md";
    if (!fs::is_regular_file(file, ec) || ec) return "";

    std::ifstream in(file, std::ios::binary);
    if (!in) return "";
    std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    const size_t totalBytes = body.size();

    // Drop a UTF-8 BOM and leading blank lines so the wrapper stays first.
    if (body.size() >= 3 && static_cast<unsigned char>(body[0]) == 0xEF &&
        static_cast<unsigned char>(body[1]) == 0xBB && static_cast<unsigned char>(body[2]) == 0xBF)
        body.erase(0, 3);
    const size_t first = body.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return ""; // empty file: inject nothing
    body.erase(0, first);

    bool truncated = false;
    if (body.size() > kAgentsMdMaxBytes) {
        body.resize(kAgentsMdMaxBytes);
        // Never cut a UTF-8 sequence in half: drop trailing continuation bytes.
        while (!body.empty() && (static_cast<unsigned char>(body.back()) & 0xC0) == 0x80)
            body.pop_back();
        truncated = true;
    }

    std::string out = AGENTS_MD_PREFIX;
    out += body;
    if (truncated) {
        // Byte counts plus where to get the rest: without them the model believes
        // it has read the whole file and works from an invisible half-document.
        out += "\n[... AGENTS.md truncated at " + std::to_string(kAgentsMdMaxBytes) + " of " +
               std::to_string(totalBytes) +
               " bytes. The rest is NOT loaded; read it directly with read_file, starting past "
               "the cut.]";
    }
    out += AGENTS_MD_SUFFIX;
    return out;
}

std::string buildProjectBlock(const std::string& workspacePath) {
    // One message, never empty: the fixed prefix must always be exactly three
    // system messages (role, environment, project), or the compaction summary and
    // the per-turn block land at a different index every conversation.
    const std::string tree = buildProjectContext(workspacePath);
    const std::string agents = buildAgentsMdContext(workspacePath);
    std::string out =
        "[PROJECT] Fixed for this session: the workspace layout, then the repository owner's "
        "instructions.\n";
    out += tree.empty() ? PROJECT_EMPTY : tree;
    out += "\n";
    out += agents.empty() ? AGENTS_EMPTY : agents;
    return out;
}
