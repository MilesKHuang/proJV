// P3, the capability map: what the model is told it can do.
//
// The map is generated per turn from the registry, so it can only be right if it
// uses the SAME allowlist rule the request uses (Agent::isToolAllowed). The tests
// below therefore cross-check the block against the Agent instead of against a
// hard-coded string, and lock down the two properties that keep the prefix cache
// alive: byte stability and sorted output.
#include "doctest.h"

#include "client/deepseek.h"
#include "core/agent.h"
#include "core/context_assembler.h"
#include "tools/file_tool.h"
#include "tools/format_tool.h"
#include "tools/md_file_tool.h"
#include "tools/registry.h"
#include "tools/shell_tool.h"
#include "tools/tool_paths.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// The real builtin registration, in the order TuiApp uses (a subset: the Agent's
// update_todo comes from its constructor, run_skill/view_image live in the TUI).
void registerBuiltins(ToolRegistry& reg, bool reversed = false) {
    if (!reversed) {
        registerFileTools(reg, ToolPaths());
        registerShellTool(reg, ToolPaths(), nullptr);
        registerFormatTool(reg, ToolPaths(), nullptr, "");
        registerMdFileTool(reg, ToolPaths());
        registerEditFileTool(reg, ToolPaths());
        registerFileSearchTool(reg, ToolPaths());
    } else {
        registerFileSearchTool(reg, ToolPaths());
        registerEditFileTool(reg, ToolPaths());
        registerMdFileTool(reg, ToolPaths());
        registerFormatTool(reg, ToolPaths(), nullptr, "");
        registerShellTool(reg, ToolPaths(), nullptr);
        registerFileTools(reg, ToolPaths());
    }
}

void writeSkill(const fs::path& dir, const std::string& name, const std::string& description,
                int agents, int rounds) {
    std::error_code ec;
    fs::create_directories(dir / name, ec);
    std::ofstream f(dir / name / "config.json", std::ios::binary);
    f << "{\"name\":\"" << name << "\",\"description\":\"" << description
      << "\",\"max_rounds\":" << rounds << ",\"agents\":[";
    for (int i = 0; i < agents; ++i) {
        if (i) f << ",";
        f << "{\"id\":\"a" << i << "\",\"display_name\":\"A" << i << "\",\"prompt\":\"p.md\"}";
    }
    f << "],\"round_steps\":[]}";
}

fs::path freshDir(const std::string& tag) {
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / ("projv_caps_" + tag);
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

} // namespace

TEST_CASE("capabilities: every registered builtin has a group of its own") {
    ToolRegistry reg;
    DeepSeekClient client;
    Agent agent(client, reg); // registers update_todo
    registerBuiltins(reg);

    for (const auto& d : reg.getToolDefinitions()) {
        // "Tool" is the fallback for pytool names the table cannot know. A builtin
        // landing there means someone added a tool and forgot the table row.
        CHECK_MESSAGE(std::string(ctx::groupOf(d.name)) != "Tool", d.name);
    }
    // The three the TUI registers are covered too, plus the pytool fallback.
    CHECK(std::string(ctx::groupOf("run_skill")) == "Skill");
    CHECK(std::string(ctx::groupOf("view_image")) == "Image");
    CHECK(std::string(ctx::groupOf("diagram_tool")) == "Tool");
}

TEST_CASE("capabilities: the map names exactly the tools the request carries") {
    ToolRegistry reg;
    DeepSeekClient client;
    Agent agent(client, reg);
    registerBuiltins(reg);

    // A role that narrows the tool list: the map must follow the same rule the
    // Agent applies, or it would advertise a tool the request does not carry.
    agent.setSystemPrompt("tools available: read_file, grep_files\n");
    REQUIRE(agent.getAllowedTools().size() == 2);

    ctx::CapabilityInputs in;
    in.tools = reg.getToolDefinitions();
    in.allowed = agent.getAllowedTools();
    const std::string block = ctx::buildCapabilities(in);

    for (const auto& d : reg.getToolDefinitions()) {
        const bool allowed = agent.isToolAllowed(d.name);
        CHECK_MESSAGE(has(block, d.name) == allowed, d.name);
    }
    CHECK(has(block, "read_file"));
    CHECK_FALSE(has(block, "exec_shell"));
}

TEST_CASE("capabilities: an allowlisted tool that does not exist is marked") {
    ToolRegistry reg;
    DeepSeekClient client;
    Agent agent(client, reg);
    registerBuiltins(reg);
    agent.setSystemPrompt("tools available: read_file, ghost_tool\n");

    ctx::CapabilityInputs in;
    in.tools = reg.getToolDefinitions();
    in.allowed = agent.getAllowedTools();
    const std::string block = ctx::buildCapabilities(in);

    CHECK(has(block, "ghost_tool (not installed)"));
    CHECK(has(block, "do not call them"));
}

TEST_CASE("capabilities: byte-identical across turns and registration order") {
    ToolRegistry a;
    DeepSeekClient ca;
    Agent aa(ca, a);
    registerBuiltins(a);

    ToolRegistry b;
    DeepSeekClient cb;
    Agent ab(cb, b);
    registerBuiltins(b, true); // same tools, different registration order

    ctx::CapabilityInputs ia;
    ia.tools = a.getToolDefinitions();
    ia.allowed = aa.getAllowedTools();
    ctx::CapabilityInputs ib;
    ib.tools = b.getToolDefinitions();
    ib.allowed = ab.getAllowedTools();

    const std::string first = ctx::buildCapabilities(ia);
    const std::string second = ctx::buildCapabilities(ia);
    const std::string reordered = ctx::buildCapabilities(ib);

    // Same input twice, and a different unordered_map iteration order: all three
    // must be the same bytes or the request prefix cache breaks every turn.
    CHECK(first == second);
    CHECK(first == reordered);
    CHECK(first.find("File") < first.find("Run"));
    CHECK(ctx::describe(first).find("tokens") != std::string::npos);
}

TEST_CASE("capabilities: skills carry a real description, then degrade") {
    const fs::path dir = freshDir("skills");
    writeSkill(dir, "alpha", "tidy the repository", 11, 10);
    writeSkill(dir, "beta", "debate a plan", 3, 8);
    writeSkill(dir, "gamma", "research a field", 9, 8);

    ToolRegistry reg;
    registerFileTools(reg, ToolPaths());
    ctx::CapabilityInputs in;
    in.tools = reg.getToolDefinitions();
    in.skills = {"alpha", "beta", "gamma"};
    in.skillsDir = dir.string();
    const std::string three = ctx::buildCapabilities(in);
    CHECK(has(three, "- alpha -- tidy the repository | 11 agents, 10 rounds"));
    CHECK(has(three, "_authoring/skill-authoring.md"));

    // Seven skills: names only, and the block says why.
    for (const char* n : {"d1", "d2", "d3", "d4"})
        writeSkill(dir, n, "a very long description that would otherwise take over", 2, 4);
    ctx::CapabilityInputs many;
    many.tools = reg.getToolDefinitions();
    many.skills = {"alpha", "beta", "gamma", "d1", "d2", "d3", "d4"};
    many.skillsDir = dir.string();
    const std::string seven = ctx::buildCapabilities(many);
    CHECK(has(seven, "7 installed"));
    CHECK_FALSE(has(seven, "tidy the repository")); // descriptions omitted
    CHECK(has(seven, "descriptions omitted"));

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("capabilities: MCP servers are listed with purpose and tool names") {
    ToolRegistry reg;
    registerFileTools(reg, ToolPaths());
    reg.registerTool(ToolDefinition{"mcp__time__convert_time", "x", {}, "{}"},
                     [](const std::string&) { return std::string(); });
    reg.registerTool(ToolDefinition{"mcp__time__get_current_time", "x", {}, "{}"},
                     [](const std::string&) { return std::string(); });

    ctx::CapabilityInputs in;
    in.tools = reg.getToolDefinitions();
    in.mcp = {{"time", "clock, timezone conversion", {"convert_time", "get_current_time"}}};
    const std::string block = ctx::buildCapabilities(in);

    CHECK(has(block, "- time (clock, timezone conversion): 2 tools -- convert_time "
                     "get_current_time"));
    // Individual mcp__ names belong to the MCP section, not to the File group.
    CHECK_FALSE(has(block, "mcp__time__convert_time"));

    // No server: one line, not a section -- and it says how to add one.
    ctx::CapabilityInputs none;
    none.tools = reg.getToolDefinitions();
    const std::string empty = ctx::buildCapabilities(none);
    CHECK(has(empty, "MCP     none running"));
    CHECK(has(empty, "mcp.json"));
}

TEST_CASE("capabilities: the block stays bounded") {
    ToolRegistry reg;
    for (int i = 0; i < 400; ++i) {
        const std::string name = "pytool_" + std::to_string(i);
        reg.registerTool(ToolDefinition{name, std::string(200, 'd'), {}, "{}"},
                         [](const std::string&) { return std::string(); });
    }
    ctx::CapabilityInputs in;
    in.tools = reg.getToolDefinitions();
    const std::string block = ctx::buildCapabilities(in);
    CHECK(block.size() <= 4200);
    CHECK(has(block, "capability map truncated"));
}
