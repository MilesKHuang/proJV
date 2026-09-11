// proJV -- project context builder implementation (extracted from legacy ui/app.cpp).
#include "project_context.h"

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr const char* PROJECT_CONTEXT_PREFIX =
    "[PROJECT CONTEXT] Workspace directory structure -- use these paths with read_file:\n"
    "Workspace: ";
constexpr const char* PROJECT_CONTEXT_SUFFIX = "\n[END PROJECT CONTEXT]";

} // namespace

std::string buildProjectContext(const std::string& workspacePath) {
    std::error_code ec;

    // Determine the root to scan.
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
        ".git", ".vs", ".vscode", ".idea",
        "build", "Debug", "Release", "x64", "x86",
        "bin", "bin64", "obj", "out", "target",
        "node_modules", "packages", "vendor", "external",
        "__pycache__", ".pytest_cache", ".mypy_cache",
        "nix", "deploy", "integrations", "fuzzing",
        "docs", "examples", "tests", "test", ".github"
    };

    static const std::vector<std::string> srcExts = {
        ".cpp", ".c", ".h", ".hpp", ".rs", ".py", ".go", ".ts", ".js",
        ".toml", ".json", ".yaml", ".yml", ".cmake", ".txt", ".md",
        "CMakeLists.txt", "Makefile", "Cargo.toml", "package.json"
    };

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

    std::ostringstream out;
    out << PROJECT_CONTEXT_PREFIX << root.string() << "\n\n";

    // Scan top-level entries.
    std::vector<fs::directory_entry> topEntries;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        topEntries.push_back(entry);
        if (ec) { ec.clear(); continue; }
    }
    std::sort(topEntries.begin(), topEntries.end(),
        [](const fs::directory_entry& a, const fs::directory_entry& b) {
            bool aDir = a.is_directory();
            bool bDir = b.is_directory();
            if (aDir != bDir) return aDir > bDir;
            return a.path().filename().string() < b.path().filename().string();
        });

    size_t totalChars = out.str().size();
    const size_t MAX_CHARS = 4500;

    int dirsListed = 0;
    for (const auto& entry : topEntries) {
        if (totalChars > MAX_CHARS) break;

        auto name = entry.path().filename().string();
        if (shouldSkip(name)) continue;

        if (entry.is_directory(ec)) {
            if (ec) { ec.clear(); continue; }
            ++dirsListed;
            out << "  " << name << "/\n";

            int subCount = 0;
            for (const auto& sub : fs::directory_iterator(entry.path(), ec)) {
                if (ec) { ec.clear(); break; }
                if (totalChars > MAX_CHARS || subCount > 40) break;

                auto subName = sub.path().filename().string();
                if (shouldSkip(subName)) continue;

                if (sub.is_directory(ec)) {
                    if (ec) { ec.clear(); continue; }
                    out << "    " << subName << "/\n";
                    ++subCount;
                    totalChars = out.str().size();

                    int fileCount = 0;
                    for (const auto& f : fs::directory_iterator(sub.path(), ec)) {
                        if (ec) { ec.clear(); break; }
                        if (totalChars > MAX_CHARS || fileCount > 30) break;

                        auto fName = f.path().filename().string();
                        if (f.is_regular_file(ec) && isSrcFile(fName)) {
                            if (ec) { ec.clear(); continue; }
                            out << "      " << fName << "\n";
                            ++fileCount;
                            totalChars = out.str().size();
                        }
                    }
                } else if (sub.is_regular_file(ec) && isSrcFile(subName)) {
                    if (ec) { ec.clear(); continue; }
                    out << "    " << subName << "\n";
                    ++subCount;
                    totalChars = out.str().size();
                }
            }
        } else if (entry.is_regular_file(ec) && isSrcFile(name)) {
            if (ec) { ec.clear(); continue; }
            out << "  " << name << "\n";
            totalChars = out.str().size();
        }
    }

    if (totalChars > MAX_CHARS) {
        out << "  ... (truncated, " << dirsListed << " dirs listed)\n";
    }

    out << PROJECT_CONTEXT_SUFFIX;
    return out.str();
}
