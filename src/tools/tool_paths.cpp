#include "tool_paths.h"

#include <cctype>
#include <filesystem>

namespace fs = std::filesystem;

const char* const kWorkspaceViolationPrefix = "__WORKSPACE_VIOLATION__:";

namespace {

std::string normalize(const std::string& p) {
    try {
        return fs::absolute(fs::path(p)).lexically_normal().string();
    } catch (...) {
        return p;
    }
}

// Windows paths are case-insensitive; POSIX ones are not.
std::string lowered(std::string s) {
    for (auto& c : s)
        c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

bool pathIsUnder(const std::string& path, const std::string& root) {
    if (root.empty()) return true;
    try {
        const fs::path base = fs::absolute(fs::path(root)).lexically_normal();
        const fs::path target = fs::path(path).lexically_normal();
        auto bit = base.begin();
        auto tit = target.begin();
        for (; bit != base.end(); ++bit, ++tit) {
            if (tit == target.end()) return false;
            std::string a = bit->string();
            std::string b = tit->string();
#ifdef _WIN32
            a = lowered(a);
            b = lowered(b);
#endif
            if (a != b) return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

ToolPaths::ToolPaths(const std::string& workspace) {
    if (!workspace.empty()) addRoot(workspace, true);
}

ToolPaths ToolPaths::forSession(const std::string& workspace, const std::string& projvFilesDir) {
    ToolPaths p;
    if (!workspace.empty()) p.addRoot(workspace, true);
    if (!projvFilesDir.empty()) {
        p.addRoot(projvFilesDir, true);
        // Sessions and logs are the user's history, not the agent's scratch space:
        // readable (so it can answer questions about them), never writable.
        p.addRoot((fs::path(projvFilesDir) / "sessions").string(), false);
        p.addRoot((fs::path(projvFilesDir) / "logs").string(), false);
    }
    return p;
}

void ToolPaths::addRoot(const std::string& path, bool writable) {
    if (path.empty()) return;
    const std::string norm = normalize(path);
    for (const auto& r : roots_) {
        if (r.path == norm) return; // no duplicates, first flag wins
    }
    if (workspace_.empty()) workspace_ = norm;
    roots_.push_back(ToolRoot{norm, writable});
}

std::string ToolPaths::resolve(const std::string& rawPath, bool forWrite,
                               std::string& outResolved) const {
    if (rawPath.empty()) return "Error: Empty path";

    std::string resolved;
    if (fs::path(rawPath).is_absolute()) {
        resolved = normalize(rawPath);
    } else if (!workspace_.empty()) {
        resolved = normalize((fs::path(workspace_) / rawPath).string());
    } else {
        resolved = normalize(rawPath);
    }

    if (roots_.empty()) { // no restriction configured (legacy / unit tests)
        outResolved = resolved;
        return "";
    }

    const ToolRoot* best = nullptr;
    for (const auto& r : roots_) {
        if (!pathIsUnder(resolved, r.path)) continue;
        if (!best || r.path.size() > best->path.size()) best = &r;
    }

    if (!best) {
        // Symlinks / junctions: a lexical check can fail on a path that really
        // does live inside a root. One canonical retry, then give up.
        try {
            std::error_code ec;
            const fs::path canonical = fs::weakly_canonical(rawPath, ec);
            if (!ec && canonical.string() != resolved) {
                for (const auto& r : roots_) {
                    if (!pathIsUnder(canonical.string(), r.path)) continue;
                    if (!best || r.path.size() > best->path.size()) best = &r;
                }
                if (best) resolved = canonical.string();
            }
        } catch (...) {
            // weakly_canonical can throw on older MSVC; a violation is the safe answer.
        }
    }

    if (!best) {
        return std::string(kWorkspaceViolationPrefix) + " Path '" + rawPath +
               "' is outside the workspace and every other root this session may touch.\n"
               "Allowed roots: " +
               describe() +
               "\nNothing outside those roots can be read or written. Use /workspace to see "
               "them.";
    }
    if (forWrite && !best->writable) {
        return std::string(kWorkspaceViolationPrefix) + " Path '" + rawPath +
               "' is read-only in this session (it is under '" + best->path +
               "').\nWritable roots: " + describe() +
               "\nRead it if you need its content; never try to write it again.";
    }
    outResolved = resolved;
    return "";
}

std::string ToolPaths::describe() const {
    if (roots_.empty()) return "(none -- everything is allowed)";
    std::string out;
    for (const auto& r : roots_) {
        if (!out.empty()) out += "; ";
        out += r.path;
        out += r.writable ? " (read+write)" : " (read only)";
    }
    return out;
}
