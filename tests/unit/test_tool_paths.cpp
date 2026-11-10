// The permission matrix: which roots a session may touch, and which of them are
// writable. This is the behaviour behind "add a skill" (projv_files is writable)
// and "never touch my history" (sessions/ and logs/ are read-only).
//
// The cases are written against the real tools, not just ToolPaths::resolve, so a
// tool that forgets to pass `forWrite` fails here instead of on a user machine.
#include "doctest.h"

#include "json.hpp"
#include "tools/registry.h"
#include "tools/file_tool.h"
#include "tools/md_file_tool.h"
#include "tools/shell_tool.h" // registerEditFileTool
#include "tools/tool_paths.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// A session with two roots, exactly like TuiApp::setupTools() builds them.
struct Sandbox {
    fs::path workspace;
    fs::path projv;
    explicit Sandbox(const std::string& tag) {
        std::error_code ec;
        const fs::path base = fs::temp_directory_path(ec) / ("projv_paths_" + tag);
        fs::remove_all(base, ec);
        workspace = base / "ws";
        projv = base / "projv_files";
        fs::create_directories(workspace, ec);
        fs::create_directories(projv / "skills", ec);
        fs::create_directories(projv / "sessions", ec);
        fs::create_directories(projv / "logs", ec);
    }
    ToolPaths paths() const { return ToolPaths::forSession(workspace.string(), projv.string()); }
};

std::string run(ToolRegistry& reg, const char* tool, const nlohmann::json& args) {
    return reg.execute(tool, args.dump());
}

} // namespace

TEST_CASE("tool paths: the matrix is workspace + projv_files, minus history") {
    const Sandbox sb("matrix");
    const ToolPaths p = sb.paths();

    REQUIRE(p.roots().size() == 4);
    CHECK(p.workspace() == fs::absolute(sb.workspace).lexically_normal().string());
    // projv_files as a whole is writable ...
    std::string out;
    CHECK(p.resolve((sb.projv / "prompts" / "me.md").string(), true, out).empty());
}

TEST_CASE("tool paths: projv_files is writable, sessions and logs are not") {
    const Sandbox sb("split");
    const ToolPaths p = sb.paths();

    std::string out;
    // A skill folder under projv_files: the exact case the old single-root guard
    // refused, which made "add a skill" impossible for the agent.
    CHECK(p.resolve((sb.projv / "skills" / "demo" / "config.json").string(), true, out).empty());
    CHECK(p.resolve((sb.projv / "mcp.json").string(), true, out).empty());

    // History: readable, never writable. The deepest matching root wins.
    CHECK(p.resolve((sb.projv / "sessions" / "abc.db").string(), false, out).empty());
    const std::string w = p.resolve((sb.projv / "sessions" / "abc.db").string(), true, out);
    CHECK(has(w, "read-only"));
    CHECK(has(w, "__WORKSPACE_VIOLATION__:"));
    CHECK(has(w, "sessions"));
    CHECK(has(p.resolve((sb.projv / "logs" / "app.log").string(), true, out), "read-only"));
}

TEST_CASE("tool paths: outside every root is refused and the message lists them") {
    const Sandbox sb("escape");
    const ToolPaths p = sb.paths();
    std::string out;

    const std::string err = p.resolve("../outside.txt", true, out);
    CHECK(has(err, "__WORKSPACE_VIOLATION__:"));
    CHECK(has(err, "outside the workspace"));
    // The roots are named, so a retry goes somewhere legal instead of the same
    // wrong path over and over.
    CHECK(has(err, fs::absolute(sb.workspace).lexically_normal().string()));
    CHECK(has(err, fs::absolute(sb.projv).lexically_normal().string()));
    CHECK(out.empty());
}

TEST_CASE("tool paths: an empty ToolPaths keeps the old unrestricted behaviour") {
    ToolPaths none;
    CHECK(none.empty());
    std::string out;
    CHECK(none.resolve("anything/at/all.txt", true, out).empty());
    CHECK(!out.empty());
}

TEST_CASE("write_file: a skill lands in projv_files, a session db does not") {
    const Sandbox sb("tools");
    ToolRegistry reg;
    registerFileTools(reg, sb.paths());

    const std::string ok = run(reg, "write_file",
                               {{"path", (sb.projv / "skills" / "demo" / "config.json").string()},
                                {"content", "{\"name\":\"demo\"}"}});
    CHECK(has(ok, "Written"));
    std::ifstream in(sb.projv / "skills" / "demo" / "config.json", std::ios::binary);
    const std::string body((std::istreambuf_iterator<char>(in)), {});
    CHECK(body == "{\"name\":\"demo\"}");

    // Writing the history is refused before a byte is touched.
    const std::string bad =
        run(reg, "write_file",
            {{"path", (sb.projv / "sessions" / "abc.db").string()}, {"content", "x"}});
    CHECK(has(bad, "read-only"));
    CHECK(has(bad, "__WORKSPACE_VIOLATION__:"));
    CHECK_FALSE(fs::exists(sb.projv / "sessions" / "abc.db"));

    // Reading it is still allowed: the agent may answer questions about history.
    const std::string rd =
        run(reg, "read_file", {{"path", (sb.projv / "sessions" / "abc.db").string()}});
    CHECK_FALSE(has(rd, "read-only"));
}

TEST_CASE("edit_file and md_file also refuse a read-only root") {
    const Sandbox sb("tools2");
    ToolRegistry reg;
    registerFileTools(reg, sb.paths());
    registerEditFileTool(reg, sb.paths());
    registerMdFileTool(reg, sb.paths());

    const fs::path target = sb.projv / "logs" / "notes.md";
    {
        std::ofstream f(target, std::ios::binary);
        f << "# log\n";
    }

    CHECK(has(run(reg, "edit_file",
                  {{"path", target.string()}, {"search", "# log"}, {"replace", "# changed"}}),
              "read-only"));
    CHECK(
        has(run(reg, "md_file", {{"action", "write"}, {"path", target.string()}, {"content", "x"}}),
            "read-only"));
    std::ifstream in(target, std::ios::binary);
    const std::string body((std::istreambuf_iterator<char>(in)), {});
    CHECK(body == "# log\n"); // untouched
}
