// Regression tests for three bugs found by running monica on this repo.
// Each of these was visible on Linux only, or was silently wrong on both.
#include "doctest.h"

#include "tools/registry.h"
#include "tools/file_tool.h"
#include "tools/md_file_tool.h"
#include "tools/shell_tool.h" // registerFileSearchTool
#include "platform/iprocess_runner.h"
#include "json.hpp"
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path freshRoot(const std::string& tag) {
    std::error_code ec;
    fs::path root = fs::temp_directory_path(ec) / ("projv_reg_" + tag);
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    return root;
}
void writeAt(const fs::path& root, const std::string& rel, const std::string& content) {
    const fs::path p = root / rel;
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary);
    f << content;
}
std::string readAt(const fs::path& root, const std::string& rel) {
    std::ifstream f(root / rel, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), {});
}
bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

} // namespace

TEST_CASE("md_file: sub-paths inside the workspace are allowed on every platform") {
    const fs::path root = freshRoot("md");
    ToolRegistry reg;
    registerMdFileTool(reg, root.string());

    // A nested path used to fail on POSIX: the guard appended a Windows '\\'.
    const std::string w = reg.execute("md_file", nlohmann::json{{"action", "write"},
                                                                {"path", "docs/design/plan.md"},
                                                                {"content", "# plan\n"}}
                                                     .dump());
    CHECK(has(w, "Written"));
    CHECK(readAt(root, "docs/design/plan.md") == "# plan\n");

    // The edit action must reach the same file.
    const std::string e = reg.execute("md_file", nlohmann::json{{"action", "edit"},
                                                                {"path", "docs/design/plan.md"},
                                                                {"search", "# plan"},
                                                                {"replace", "# plan v2"}}
                                                     .dump());
    CHECK(has(e, "Edited"));
    CHECK(readAt(root, "docs/design/plan.md") == "# plan v2\n");

    // Still refuses to escape the workspace.
    CHECK(has(
        reg.execute(
            "md_file",
            nlohmann::json{{"action", "write"}, {"path", "../escape.md"}, {"content", "x"}}.dump()),
        "outside the workspace"));
    // ... and still refuses non-.md files.
    CHECK(has(
        reg.execute(
            "md_file",
            nlohmann::json{{"action", "write"}, {"path", "notes.txt"}, {"content", "x"}}.dump()),
        "must end with .md"));
}

TEST_CASE("grep_files: a file path searches that file instead of returning nothing") {
    const fs::path root = freshRoot("grep_file");
    writeAt(root, "src/a.cpp", "void keepMe() {}\nvoid other() {}\n");
    ToolRegistry reg;
    registerFileTools(reg, root.string());

    // Used to answer "No matches found", which reads as "no callers".
    const std::string out = reg.execute(
        "grep_files", nlohmann::json{{"pattern", "keepMe"}, {"path", "src/a.cpp"}}.dump());
    CHECK(has(out, "keepMe"));
    CHECK(has(out, "a.cpp:1"));
    CHECK(!has(out, "No matches found"));

    // No hit in a single file still says so, and does not fall through to a walk.
    const std::string miss = reg.execute(
        "grep_files", nlohmann::json{{"pattern", "absent"}, {"path", "src/a.cpp"}}.dump());
    CHECK(has(miss, "No matches found"));
}

TEST_CASE("process runner: extraEnv reaches the child on this platform") {
    // process_runner_win.cpp used to ignore ProcessConfig::extraEnv entirely, so
    // pytool silently lost its environment variables on Windows.
    IProcessRunner* runner = CreateProcessRunner();
    REQUIRE(runner != nullptr);

    ProcessConfig cfg;
#ifdef _WIN32
    // A child process must inherit the value; note that `set X=1 && echo %X%`
    // does NOT work on Windows (cmd expands %X% before the set runs).
    cfg.command = "powershell -NoProfile -Command \"$env:PROJV_TESTVAR\"";
#else
    cfg.command = "echo $PROJV_TESTVAR";
#endif
    cfg.extraEnv["PROJV_TESTVAR"] = "hello-env";
    cfg.timeoutMs = 60000;

    const ProcessResult res = runner->Run(cfg);
    runner->Cancel();
    delete runner;

    CHECK(res.exitCode == 0);
    CHECK(has(res.stdout_, "hello-env"));
}

TEST_CASE("file_search: stays inside the workspace and skips junk directories") {
    const fs::path root = freshRoot("fsearch");
    writeAt(root, "src/core/agent.cpp", "x");
    writeAt(root, "src/notes.txt", "x");
    writeAt(root, "build/generated.cpp", "x");
    writeAt(root, "node_modules/pkg/index.js", "x");
    writeAt(root, ".git/config", "x");

    ToolRegistry reg;
    registerFileSearchTool(reg, root.string());

    // Default path is the workspace root, and build/ node_modules/ .git/ are
    // skipped -- before this fix the tool walked all of them.
    const std::string all = reg.execute("file_search", nlohmann::json{{"pattern", "*.cpp"}}.dump());
    CHECK(has(all, "agent.cpp"));
    CHECK(!has(all, "generated.cpp"));
    CHECK(!has(all, "node_modules"));

    // Nested path inside the workspace works.
    const std::string scoped =
        reg.execute("file_search", nlohmann::json{{"pattern", "*.txt"}, {"path", "src"}}.dump());
    CHECK(has(scoped, "notes.txt"));

    // Outside the workspace is refused, exactly like read_file.
    CHECK(has(
        reg.execute("file_search",
                    nlohmann::json{{"pattern", "*.cpp"}, {"path", "/definitely/not/here"}}.dump()),
        "outside the workspace"));

    // A dot in a pattern is literal, not "any character".
    CHECK(has(reg.execute("file_search", nlohmann::json{{"pattern", "notes.txt"}}.dump()),
              "notes.txt"));
    CHECK(has(reg.execute("file_search", nlohmann::json{{"pattern", "notesXtxt"}}.dump()),
              "No files found"));
}
