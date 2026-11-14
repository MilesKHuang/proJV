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

TEST_CASE("capabilities: no global truncation, every group survives") {
    // The old rule cut the block at 4000 chars from the tail, which is where
    // Skills and MCP sit -- so a well-configured project lost exactly the parts it
    // had configured. Every section is bounded on its own now.
    ToolRegistry reg;
    for (int i = 0; i < 400; ++i) {
        const std::string name = "pytool_" + std::to_string(i);
        reg.registerTool(ToolDefinition{name, std::string(200, 'd'), {}, "{}"},
                         [](const std::string&) { return std::string(); });
    }
    ctx::CapabilityInputs in;
    in.tools = reg.getToolDefinitions();
    in.skills = {"alpha", "beta", "gamma"};
    in.mcp = {{"big",
               "a server with many tools",
               {"t1", "t2", "t3", "t4", "t5", "t6", "t7", "t8", "t9", "t10", "t11", "t12"}}};
    const std::string block = ctx::buildCapabilities(in);
    CHECK_FALSE(has(block, "capability map truncated"));
    CHECK(has(block, "MCP     1 server(s)"));
    CHECK(has(block, "pytool_399")); // the last-registered tool still shows up
    CHECK(ctx::groupOf("pytool_250") != nullptr);
}

TEST_CASE("capabilities: MCP tool names are capped at ten per server") {
    ToolRegistry reg;
    registerFileTools(reg, ToolPaths());
    ctx::CapabilityInputs in;
    in.tools = reg.getToolDefinitions();
    std::vector<std::string> names;
    for (int i = 1; i <= 12; ++i)
        names.push_back("tool" + std::to_string(i));
    in.mcp = {{"big", "many tools", names}};
    const std::string block = ctx::buildCapabilities(in);

    CHECK(has(block, ": 12 tools -- tool1 tool2"));
    CHECK(has(block, "tool10 (+2 more)"));
    CHECK_FALSE(has(block, "tool11")); // beyond the cap
    CHECK_FALSE(has(block, "tool12"));

    // A server at or under the cap keeps the plain listing, with no "(+0 more)".
    in.mcp = {{"small", "", {"a", "b"}}};
    const std::string small = ctx::buildCapabilities(in);
    CHECK(has(small, ": 2 tools -- a b"));
    CHECK_FALSE(has(small, "more)"));
}

TEST_CASE("capabilities: an extreme but plausible setup stays under 8 KB") {
    // Soft ceiling: not enforced in code, asserted here so a future unbounded
    // section is caught before it eats the conversation. Scenario: every builtin,
    // 8 skills, 10 MCP servers with 20 tools each -- the shape the old 4000-char
    // cut was silently mangling.
    ToolRegistry reg;
    registerBuiltins(reg);
    ctx::CapabilityInputs in;
    in.tools = reg.getToolDefinitions();
    in.skillsDir = "."; // descriptions read as empty; the count still renders
    for (int s = 0; s < 8; ++s)
        in.skills.push_back("skill" + std::to_string(s));
    for (int s = 0; s < 10; ++s) {
        ctx::McpServerInfo st;
        st.name = "server" + std::to_string(s);
        st.description = "an MCP server that does something useful";
        for (int t = 0; t < 20; ++t)
            st.tools.push_back("tool_name_" + std::to_string(t));
        in.mcp.push_back(st);
    }
    const std::string block = ctx::buildCapabilities(in);
    CHECK(block.size() <= 8000);
    CHECK(has(block, "(+10 more)"));
    CHECK(has(block, "MCP     10 server(s)"));
    CHECK(has(block, "Skills  8 installed"));
}
