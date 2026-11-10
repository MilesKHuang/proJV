#pragma once
#include <string>
#include <vector>

// The set of filesystem roots this session may touch, and which of them are
// writable. One object, injected into every file-touching tool, replacing the
// single `workspacePath` string those tools used to carry.
//
// Why it exists: projv_files lives next to the exe, the workspace is the launch
// directory, and the old guard only accepted paths under the workspace -- so the
// agent could not write projv_files/prompts or skills/, which is exactly what a
// request like "add a skill" needs. The permission matrix is now data, not code.
//
// Resolution rules:
//   - A relative path is taken against workspace() (the first root added).
//   - The DEEPEST matching root decides, so a read-only sub-root can sit inside
//     a writable one (projv_files/sessions inside projv_files).
//   - An object with no roots allows everything: that is the legacy behaviour the
//     tools had with an empty workspace string, and the unit tests rely on it.
struct ToolRoot {
    std::string path; // absolute, lexically normal
    bool writable = true;
};

class ToolPaths {
  public:
    ToolPaths() = default;
    // One writable root. Implicit on purpose: `ToolPaths(root)` keeps test call
    // sites one token away from the old string they used to pass.
    ToolPaths(const std::string& workspace); // NOLINT: implicit by design

    // workspace (read+write) + projv_files sub-roots: prompts / skills / theme /
    // pytool / mcp.json are writable, sessions and logs are read-only.
    static ToolPaths forSession(const std::string& workspace, const std::string& projvFilesDir);

    void addRoot(const std::string& path, bool writable = true);

    const std::vector<ToolRoot>& roots() const { return roots_; }
    // The root relative paths are resolved against; empty when there is none.
    const std::string& workspace() const { return workspace_; }
    bool empty() const { return roots_.empty(); }

    // Resolve `rawPath` and check it against every root. Returns "" and fills
    // `outResolved` on success; on violation returns the text handed to the model
    // (always starting with __WORKSPACE_VIOLATION__: and listing the roots, so a
    // retry goes to a legal path instead of the same wrong one).
    std::string resolve(const std::string& rawPath, bool forWrite, std::string& outResolved) const;

    // "D:\ws (read+write); C:\x\projv_files (read+write); ... (read only)".
    std::string describe() const;

  private:
    std::vector<ToolRoot> roots_;
    std::string workspace_;
};

// True when `path` is `root` itself or lives under it. Component-wise, so
// "D:\ws" never contains "D:\ws2" and the separator style does not matter.
bool pathIsUnder(const std::string& path, const std::string& root);

// The marker Agent::run() matches to raise the "outside the allowed roots"
// warning. Kept as a literal contract between the tools and the agent.
extern const char* const kWorkspaceViolationPrefix;
