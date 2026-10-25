// P1 of 05_SKILL_IMPROVE.md: file tools.
// read_file line ranges, list_dir, grep_files limits, format_file.
// No network, no UI, temp workspace only.
#include "doctest.h"

#include "tools/registry.h"
#include "tools/file_tool.h"
#include "tools/format_tool.h"
#include "platform/iprocess_runner.h"
#include "json.hpp"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path freshRoot(const std::string& tag) {
    std::error_code ec;
    fs::path root = fs::temp_directory_path(ec) / ("projv_ft_" + tag);
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

std::string run(ToolRegistry& reg, const std::string& tool, const nlohmann::json& a) {
    return reg.execute(tool, a.dump());
}

bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

} // namespace

TEST_CASE("file tools: read_file without a range keeps the legacy behaviour") {
    const fs::path root = freshRoot("read_whole");
    writeAt(root, "a.txt", "one\ntwo\nthree\n");
    ToolRegistry reg;
    registerFileTools(reg, root.string());

    CHECK(run(reg, "read_file", {{"path", "a.txt"}}) == "one\ntwo\nthree\n");
}

TEST_CASE("file tools: read_file range returns a header and numbered lines") {
    const fs::path root = freshRoot("read_range");
    writeAt(root, "b.txt", "l1\nl2\nl3\nl4\nl5\n");
    ToolRegistry reg;
    registerFileTools(reg, root.string());

    const std::string out =
        run(reg, "read_file", {{"path", "b.txt"}, {"start_line", 2}, {"end_line", 4}});
    CHECK(out.rfind("[lines 2-4 of 5]", 0) == 0);
    CHECK(has(out, "2: l2"));
    CHECK(has(out, "4: l4"));
    CHECK(!has(out, "5: l5"));
    CHECK(!has(out, "1: l1"));
}

TEST_CASE("file tools: read_file accepts one-sided ranges and numeric strings") {
    const fs::path root = freshRoot("read_onesided");
    writeAt(root, "c.txt", "a\nb\nc\nd\n");
    ToolRegistry reg;
    registerFileTools(reg, root.string());

    const std::string from3 = run(reg, "read_file", {{"path", "c.txt"}, {"start_line", 3}});
    CHECK(from3.rfind("[lines 3-4 of 4]", 0) == 0);
    CHECK(has(from3, "3: c"));
    CHECK(has(from3, "4: d"));

    const std::string to2 = run(reg, "read_file", {{"path", "c.txt"}, {"end_line", 2}});
    CHECK(to2.rfind("[lines 1-2 of 4]", 0) == 0);
    CHECK(has(to2, "2: b"));

    // Some models send the line numbers as JSON strings.
    const std::string strArgs =
        nlohmann::json{{"path", "c.txt"}, {"start_line", "2"}, {"end_line", "3"}}.dump();
    CHECK(reg.execute("read_file", strArgs).rfind("[lines 2-3 of 4]", 0) == 0);
}

TEST_CASE("file tools: read_file rejects bad ranges with a clear message") {
    const fs::path root = freshRoot("read_bad");
    writeAt(root, "d.txt", "1\n2\n3\n");
    ToolRegistry reg;
    registerFileTools(reg, root.string());

    const std::string reversed =
        run(reg, "read_file", {{"path", "d.txt"}, {"start_line", 3}, {"end_line", 2}});
    CHECK(has(reversed, "must be >= start_line"));

    const std::string beyond = run(reg, "read_file", {{"path", "d.txt"}, {"start_line", 99}});
    CHECK(has(beyond, "beyond the end of the file"));
    CHECK(has(beyond, "3 lines"));
}

TEST_CASE("file tools: read_file truncates at 1000 lines and points at the next start") {
    const fs::path root = freshRoot("read_trunc");
    std::string body;
    for (int i = 1; i <= 1500; ++i)
        body += "line" + std::to_string(i) + "\n";
    writeAt(root, "big.txt", body);
    ToolRegistry reg;
    registerFileTools(reg, root.string());

    // Range mode (start_line given): capped at 1000 lines per reply.
    const std::string ranged = run(reg, "read_file", {{"path", "big.txt"}, {"start_line", 1}});
    CHECK(ranged.rfind("[lines 1-1500 of 1500]", 0) == 0);
    CHECK(has(ranged, "[truncated, continue with start_line=1001]"));

    // Legacy mode (no range args) is untouched: whole file, 100KB guard only.
    const std::string whole = run(reg, "read_file", {{"path", "big.txt"}});
    CHECK(!has(whole, "continue with start_line"));
    CHECK(whole.rfind("1: line1", 0) != 0); // no line numbers in legacy mode
}

TEST_CASE("file tools: read_file range bypasses the 100KB whole-file limit") {
    const fs::path root = freshRoot("read_bigrange");
    std::string body;
    for (int i = 0; i < 6000; ++i)
        body += "0123456789abcdef0123456789\n"; // ~150KB
    writeAt(root, "wide.txt", body);
    ToolRegistry reg;
    registerFileTools(reg, root.string());

    CHECK(has(run(reg, "read_file", {{"path", "wide.txt"}}), "File too large"));
    const std::string piece =
        run(reg, "read_file", {{"path", "wide.txt"}, {"start_line", 5000}, {"end_line", 5010}});
    CHECK(piece.rfind("[lines 5000-5010 of 6000]", 0) == 0);
    CHECK(has(piece, "5000: 0123456789abcdef0123456789"));
}

TEST_CASE("file tools: parseFileRequestSpec splits path and line range") {
    std::string p;
    int s = 0, e = 0;

    CHECK(parseFileRequestSpec("src/a.cpp", p, s, e));
    CHECK(p == "src/a.cpp");
    CHECK(s == 0);
    CHECK(e == 0);

    CHECK(parseFileRequestSpec("src/a.cpp#L120-L480", p, s, e));
    CHECK(p == "src/a.cpp");
    CHECK(s == 120);
    CHECK(e == 480);

    CHECK(parseFileRequestSpec("src/a.cpp#L120", p, s, e));
    CHECK(p == "src/a.cpp");
    CHECK(s == 120);
    CHECK(e == 0);

    CHECK(parseFileRequestSpec("src/a.cpp#l7-l9", p, s, e)); // lowercase
    CHECK(p == "src/a.cpp");
    CHECK(s == 7);
    CHECK(e == 9);

    // A '#' that is not a line range belongs to the path.
    CHECK(parseFileRequestSpec("weird#name.cpp", p, s, e));
    CHECK(p == "weird#name.cpp");
    CHECK(s == 0);

    CHECK(!parseFileRequestSpec("", p, s, e));
}

TEST_CASE("file tools: list_dir expands to depth and annotates unexpanded dirs") {
    const fs::path root = freshRoot("list");
    writeAt(root, "src/core/a.cpp", "x");
    writeAt(root, "src/b.h", "y");
    writeAt(root, "build/artifact.obj", "z");
    writeAt(root, "node_modules/pkg/index.js", "w");
    writeAt(root, ".hidden/secret.txt", "s");
    ToolRegistry reg;
    registerFileTools(reg, root.string());

    const std::string out = run(reg, "list_dir", {{"depth", 2}});
    CHECK(has(out, "[list_dir]"));
    CHECK(has(out, "src/"));
    CHECK(has(out, "  core/ (1 entries, not expanded)"));
    CHECK(has(out, "  b.h"));
    CHECK(!has(out, "build/"));        // kSkipDirs
    CHECK(!has(out, "node_modules/")); // kSkipDirs
    CHECK(!has(out, ".hidden/"));      // dot-directories
    CHECK(!has(out, "a.cpp"));         // depth 2 stops above it

    const std::string deeper = run(reg, "list_dir", {{"path", "src"}, {"depth", 3}});
    CHECK(has(deeper, "core/"));
    CHECK(has(deeper, "  a.cpp"));
}

TEST_CASE("file tools: list_dir honours exclude and rejects a bad regex") {
    const fs::path root = freshRoot("list_exclude");
    writeAt(root, "src/a.cpp", "x");
    writeAt(root, "src/b.h", "y");
    writeAt(root, "src/c.png", "z");
    ToolRegistry reg;
    registerFileTools(reg, root.string());

    const std::string out = run(reg, "list_dir", {{"depth", 3}, {"exclude", "\\.(h|png)$"}});
    CHECK(has(out, "a.cpp"));
    CHECK(!has(out, "b.h"));
    CHECK(!has(out, "c.png"));

    CHECK(has(run(reg, "list_dir", {{"exclude", "([unclosed"}}), "invalid 'exclude' regex"));
}

TEST_CASE("file tools: list_dir rejects paths outside the workspace") {
    const fs::path root = freshRoot("list_outside");
    ToolRegistry reg;
    registerFileTools(reg, root.string());

    // The workspace root itself must be allowed (default path).
    CHECK(!has(run(reg, "list_dir", nlohmann::json::object()), "outside the workspace"));
    CHECK(has(run(reg, "list_dir", {{"path", "/definitely/not/here"}}), "outside the workspace"));
}

TEST_CASE("file tools: list_dir stops at the entry cap") {
    const fs::path root = freshRoot("list_cap");
    for (int i = 0; i < 60; ++i) {
        for (int j = 0; j < 10; ++j)
            writeAt(root, "d" + std::to_string(i) + "/f" + std::to_string(j) + ".txt", "x");
    }
    ToolRegistry reg;
    registerFileTools(reg, root.string());

    const std::string out = run(reg, "list_dir", {{"depth", 6}});
    CHECK(has(out, "[truncated at 500 entries"));
}

TEST_CASE("file tools: grep_files honours max_results and flags truncation") {
    const fs::path root = freshRoot("grep");
    std::string body;
    for (int i = 0; i < 10; ++i)
        body += "needle line " + std::to_string(i) + "\n";
    writeAt(root, "src/g.txt", body);
    ToolRegistry reg;
    registerFileTools(reg, root.string());

    const std::string capped = run(reg, "grep_files", {{"pattern", "needle"}, {"max_results", 3}});
    CHECK(has(capped, "[truncated at 3 results"));

    // Default limit is 100: all ten hits, no truncation notice. No path arg ->
    // the workspace root is the search root.
    const std::string all = run(reg, "grep_files", {{"pattern", "needle"}});
    CHECK(!has(all, "[truncated at"));
    CHECK(has(all, "needle line 9"));

    // Out-of-range values are clamped, not rejected.
    CHECK(has(run(reg, "grep_files", {{"pattern", "needle"}, {"max_results", 9999}}),
              "needle line 9"));
    CHECK(
        has(run(reg, "grep_files", {{"pattern", "needle"}, {"max_results", 0}}), "needle line 9"));
}

TEST_CASE("format_file: whitespace mode cleans indentation and trailing spaces") {
    const fs::path root = freshRoot("fmt_ws");
    writeAt(root, "w.cpp", "\tif (x) {   \n\t\treturn 1;\n\n\n");
    ToolRegistry reg;
    registerFileTools(reg, root.string());
    registerFormatTool(reg, root.string(), nullptr, "");

    const std::string out = run(reg, "format_file", {{"path", "w.cpp"}, {"mode", "whitespace"}});
    CHECK(has(out, "Formatted (whitespace)"));
    CHECK(readAt(root, "w.cpp") == "    if (x) {\n        return 1;\n");

    // Idempotent: a second pass reports no changes.
    CHECK(has(run(reg, "format_file", {{"path", "w.cpp"}, {"mode", "whitespace"}}), "No changes"));
}

TEST_CASE("format_file: whitespace mode preserves CRLF and does not touch code") {
    const fs::path root = freshRoot("fmt_crlf");
    writeAt(root, "c.cpp", "int  a = 1;\r\nint b = 2;   \r\n");
    ToolRegistry reg;
    registerFileTools(reg, root.string());
    registerFormatTool(reg, root.string(), nullptr, "");

    run(reg, "format_file", {{"path", "c.cpp"}, {"mode", "whitespace"}});
    CHECK(readAt(root, "c.cpp") == "int  a = 1;\r\nint b = 2;\r\n");
}

TEST_CASE("format_file: clang mode reports a clear error and leaves the file alone") {
    const fs::path root = freshRoot("fmt_clang");
    const std::string original = "int   x = 1;\n";
    writeAt(root, "n.cpp", original);

    {
        ToolRegistry reg;
        registerFileTools(reg, root.string());
        registerFormatTool(reg, root.string(), nullptr, "");
        CHECK(has(run(reg, "format_file", {{"path", "n.cpp"}}), "No process runner"));
    }
    if (IProcessRunner* runner = CreateProcessRunner()) {
        ToolRegistry reg;
        registerFileTools(reg, root.string());
        registerFormatTool(reg, root.string(), runner, "projv_no_such_clang_format");
        // An explicit style gets past the "no .clang-format" guard, so the
        // missing binary is what we exercise here.
        const std::string out = run(reg, "format_file", {{"path", "n.cpp"}, {"style", "LLVM"}});
        CHECK(has(out, "not found"));
        CHECK(has(out, "mode=\"whitespace\"")); // points at the fallback
        CHECK(readAt(root, "n.cpp") == original);
        runner->Cancel();
        delete runner;
    }
}

TEST_CASE("format_file: unknown mode is rejected") {
    const fs::path root = freshRoot("fmt_bad");
    writeAt(root, "x.cpp", "int x;\n");
    ToolRegistry reg;
    registerFileTools(reg, root.string());
    registerFormatTool(reg, root.string(), nullptr, "");
    CHECK(has(run(reg, "format_file", {{"path", "x.cpp"}, {"mode", "prettier"}}), "Unknown mode"));
}

TEST_CASE("format_file: clang mode refuses to guess a style when there is no .clang-format") {
    const fs::path root = freshRoot("fmt_nostyle");
    writeAt(root, "a.cpp", "int  x = 1;\n");
    ToolRegistry reg;
    registerFileTools(reg, root.string());
    // A runner is present, so the only thing stopping the call is the missing
    // style file -- which is the point: LLVM fallback would restyle everything.
    registerFormatTool(reg, root.string(), CreateProcessRunner(), "");

    const std::string out = run(reg, "format_file", {{"path", "a.cpp"}});
    CHECK(has(out, "no .clang-format found"));
    CHECK(has(out, "mode=\"whitespace\""));
    CHECK(readAt(root, "a.cpp") == "int  x = 1;\n"); // untouched
}

TEST_CASE("format_file: clang mode works when a style is given explicitly") {
    // Skips itself when this machine has no clang-format binary.
    std::vector<std::string> candidates = {
#ifdef _WIN32
        "C:\\Program Files (x86)\\Microsoft Visual "
        "Studio\\2019\\Community\\VC\\Tools\\Llvm\\x64\\bin\\clang-format.exe",
        "C:\\Program Files\\Microsoft Visual "
        "Studio\\2022\\Community\\VC\\Tools\\Llvm\\x64\\bin\\clang-format.exe",
        "C:\\Program Files\\LLVM\\bin\\clang-format.exe",
#else
        "/usr/bin/clang-format",
        "/usr/local/bin/clang-format",
#endif
    };
    std::string exe;
    for (const auto& c : candidates) {
        std::error_code ec;
        if (fs::exists(c, ec)) {
            exe = c;
            break;
        }
    }
    if (exe.empty()) {
        WARN("no clang-format on this machine -- clang branch not exercised");
        return;
    }

    const fs::path root = freshRoot("fmt_clang_real");
    writeAt(root, "b.cpp", "int    main(){return 0;}   \n");
    ToolRegistry reg;
    registerFileTools(reg, root.string());
    IProcessRunner* runner = CreateProcessRunner();
    REQUIRE(runner != nullptr);
    registerFormatTool(reg, root.string(), runner, exe);

    const std::string out = run(reg, "format_file", {{"path", "b.cpp"}, {"style", "LLVM"}});
    runner->Cancel();
    delete runner;

    CHECK(has(out, "Formatted (clang-format)"));
    const std::string after = readAt(root, "b.cpp");
    CHECK(after != "int    main(){return 0;}   \n"); // it really rewrote the file
    CHECK(has(after, "int main()"));
}
