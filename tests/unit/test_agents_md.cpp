// AGENTS.md loading: the workspace root file becomes one fixed system message,
// wrapped in [AGENTS.MD] ... [END AGENTS.MD]. Only the root file is read, it is
// capped at 16 KiB, and a missing or empty file injects nothing at all.
#include "doctest.h"

#include "core/project_context.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path makeTempWorkspace(const std::string& tag) {
    std::error_code ec;
    fs::path root = fs::temp_directory_path(ec) / ("projv_agentsmd_" + tag);
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    return root;
}

void writeFile(const fs::path& file, const std::string& content) {
    std::error_code ec;
    if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary);
    out << content;
}

} // namespace

TEST_CASE("AGENTS.md: missing file injects nothing") {
    const fs::path ws = makeTempWorkspace("missing");
    CHECK(buildAgentsMdContext(ws.string()).empty());
    std::error_code ec;
    fs::remove_all(ws, ec);
}

TEST_CASE("AGENTS.md: empty file injects nothing") {
    const fs::path ws = makeTempWorkspace("empty");
    writeFile(ws / "AGENTS.md", "\n\n   \n");
    CHECK(buildAgentsMdContext(ws.string()).empty());
    std::error_code ec;
    fs::remove_all(ws, ec);
}

TEST_CASE("AGENTS.md: content is wrapped and kept verbatim") {
    const fs::path ws = makeTempWorkspace("normal");
    writeFile(ws / "AGENTS.md", "# Rules\n\nRun ctest. Do not touch external/.\n");
    const std::string ctx = buildAgentsMdContext(ws.string());

    CHECK(ctx.find("[AGENTS.MD]") == 0); // wrapper is first
    CHECK(ctx.find("[END AGENTS.MD]") != std::string::npos);
    CHECK(ctx.find("Run ctest. Do not touch external/.") != std::string::npos);
    CHECK(ctx.find("truncated") == std::string::npos);
    // The wrapper text tells the model how to treat the block.
    CHECK(ctx.find("Project instructions from the repository owner") != std::string::npos);
    std::error_code ec;
    fs::remove_all(ws, ec);
}

TEST_CASE("AGENTS.md: UTF-8 BOM and leading blank lines are dropped") {
    const fs::path ws = makeTempWorkspace("bom");
    writeFile(ws / "AGENTS.md", "\xef\xbb\xbf\n\n# 规则\n用中文写提交说明。\n");
    const std::string ctx = buildAgentsMdContext(ws.string());

    CHECK(ctx.find("otherwise.\n# ") != std::string::npos); // BOM gone, heading first
    CHECK(ctx.find("\xef\xbb\xbf") == std::string::npos);
    CHECK(ctx.find("\xe7\x94\xa8\xe4\xb8\xad\xe6\x96\x87") != std::string::npos); // 用中文
    std::error_code ec;
    fs::remove_all(ws, ec);
}

TEST_CASE("AGENTS.md: files over 16 KiB are truncated with a marker") {
    const fs::path ws = makeTempWorkspace("big");
    // 20 KiB of ASCII, so the cut never lands inside a UTF-8 sequence.
    std::string body(20 * 1024, 'x');
    writeFile(ws / "AGENTS.md", body);

    const std::string ctx = buildAgentsMdContext(ws.string());
    CHECK(ctx.find("[... truncated, read AGENTS.md for the rest]") != std::string::npos);
    // Wrapper + 16 KiB cap + tail marker: comfortably below the raw 20 KiB.
    CHECK(ctx.size() < body.size());
    std::error_code ec;
    fs::remove_all(ws, ec);
}

TEST_CASE("AGENTS.md: a sub-directory AGENTS.md is ignored") {
    const fs::path ws = makeTempWorkspace("subdir");
    writeFile(ws / "sub" / "AGENTS.md", "# nested\n");
    CHECK(buildAgentsMdContext(ws.string()).empty());
    std::error_code ec;
    fs::remove_all(ws, ec);
}

TEST_CASE("project context and AGENTS.md are independent blocks") {
    const fs::path ws = makeTempWorkspace("both");
    writeFile(ws / "AGENTS.md", "# Rules\n");
    writeFile(ws / "main.cpp", "int main() { return 0; }\n");

    const std::string proj = buildProjectContext(ws.string());
    const std::string agents = buildAgentsMdContext(ws.string());
    CHECK(proj.find("[PROJECT CONTEXT]") == 0);
    CHECK(proj.find("AGENTS.MD") == std::string::npos);
    CHECK(agents.find("[AGENTS.MD]") == 0);
    CHECK(agents.find("PROJECT CONTEXT") == std::string::npos);
    std::error_code ec;
    fs::remove_all(ws, ec);
}
