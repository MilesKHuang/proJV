// Engine capability tests: tool_mode=auto (capability tools reachable),
// configurable max_tool_iters, and stop_when early stop. Scripted responders.
#include "doctest.h"

#include "core/skill_runner.h"
#include "core/config.h"
#include "tools/registry.h"
#include "platform/isystem_util.h"
#include "json.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

void ensureSysUtil() {
    static bool once = [] { SystemUtil::Init(CreateSystemUtil()); return true; }();
    (void)once;
}
ToolCall makeCall(const std::string& name, const nlohmann::json& args) {
    ToolCall tc; tc.id = "call_" + name; tc.name = name;
    tc.arguments = args.dump(); tc.index = 0; tc.valid = true; return tc;
}
AppConfig testConfig() { AppConfig c; c.model = "deepseek-v4-flash"; return c; }

// Writes a temp skill (config + tools/emit.json) and returns the skills root.
std::string makeSkill(const std::string& name, const std::string& cfg) {
    std::error_code ec;
    std::string root = getProjvDir() + "/_enginetest";
    std::string sd = root + "/" + name;
    std::filesystem::create_directories(sd + "/tools", ec);
    { std::ofstream f(sd + "/config.json"); f << cfg; }
    { std::ofstream f(sd + "/tools/emit.json");
      f << R"JSON({"name":"emit","parameters":[{"name":"summary","type":"string","required":true}]})JSON"; }
    return root;
}

const char* CFG_AUTO = R"JSON({
  "name": "t_auto",
  "tools": ["cap"],
  "tool_mode": "auto",
  "max_tool_iters": 5,
  "max_rounds": 1,
  "converge": "none",
  "agents": [{"id": "a", "display_name": "A", "prompt": "a.md"}],
  "round_steps": [
    {"actions": [{"agent": "a", "tool": "emit", "capture": "summary", "into": "a", "emit": "message", "message": "go"}]}
  ]
}
)JSON";

const char* CFG_STOP = R"JSON({
  "name": "t_stop",
  "tool_mode": "auto",
  "max_rounds": 5,
  "converge": "none",
  "stop_when": {"slot": "a", "equals": "STOP"},
  "agents": [{"id": "a", "display_name": "A", "prompt": "a.md"}],
  "round_steps": [
    {"actions": [{"agent": "a", "tool": "emit", "capture": "summary", "into": "a", "emit": "message", "message": "r{{round}}"}]}
  ]
}
)JSON";

const char* CFG_MULTI = R"JSON({
  "name": "t_multi",
  "tools": ["cap_a", "cap_b"],
  "tool_mode": "auto",
  "max_tool_iters": 5,
  "max_rounds": 1,
  "converge": "none",
  "agents": [{"id": "a", "display_name": "A", "prompt": "a.md"}],
  "round_steps": [
    {"actions": [{"agent": "a", "tool": "emit", "capture": "summary", "into": "a", "emit": "message", "message": "go"}]}
  ]
}
)JSON";

} // namespace

TEST_CASE("engine: tool_mode=auto reaches capability tools, max_tool_iters honored") {
    ensureSysUtil();
    std::string root = makeSkill("t_auto", CFG_AUTO);

    ToolRegistry reg;
    int capCalls = 0;
    { ToolDefinition d; d.name = "cap"; d.description = "cap";
      reg.registerTool(d, [&capCalls](const std::string&) { ++capCalls; return std::string("cap-ok"); }); }

    int n = 0;
    auto a = [&n](const std::string&, const std::vector<Message>&) {
        if (++n <= 4) return makeCall("cap", nlohmann::json{{"i", n}});   // 4 capability calls
        return makeCall("emit", nlohmann::json{{"summary", "done"}});
    };
    std::vector<std::string> events;
    SkillRunner::Callbacks cbs;
    cbs.onEvent = [&events](const std::string& r, const std::string& k, const std::string& t) {
        events.push_back(r + "|" + k + "|" + t);
    };
    SkillRunner sr(testConfig(), &reg, cbs, "", root);
    sr.setResponder("a", a);
    sr.run("t_auto", "T");

    CHECK(capCalls == 4);   // max_tool_iters=5 allowed 4 caps + 1 emit
    bool found = false;
    for (auto& e : events) if (e == "A|message|done") found = true;
    CHECK(found);
}

TEST_CASE("engine: stop_when halts the round loop early") {
    ensureSysUtil();
    std::string root = makeSkill("t_stop", CFG_STOP);
    auto a = [](const std::string&, const std::vector<Message>&) {
        return makeCall("emit", nlohmann::json{{"summary", "STOP"}});   // slot a == "STOP"
    };
    SkillRunner::Callbacks cbs;
    SkillRunner sr(testConfig(), nullptr, cbs, "", root);
    sr.setResponder("a", a);
    sr.run("t_stop", "T");
    CHECK(sr.lastRounds() == 1);   // stopped at round 1, not max_rounds=5
}

TEST_CASE("engine: a response with multiple tool_calls executes ALL of them") {
    ensureSysUtil();
    std::string root = makeSkill("t_multi", CFG_MULTI);

    ToolRegistry reg;
    int a = 0, b = 0;
    { ToolDefinition d; d.name = "cap_a"; d.description = "a";
      reg.registerTool(d, [&a](const std::string&) { ++a; return std::string("a"); }); }
    { ToolDefinition d; d.name = "cap_b"; d.description = "b";
      reg.registerTool(d, [&b](const std::string&) { ++b; return std::string("b"); }); }

    int n = 0;
    SkillRunner::Callbacks cbs;
    SkillRunner sr(testConfig(), &reg, cbs, "", root);
    sr.setResponderMulti("a", [&n](const std::string&, const std::vector<Message>&) -> std::vector<ToolCall> {
        if (++n == 1) return { makeCall("cap_a", nlohmann::json{{"i", 1}}), makeCall("cap_b", nlohmann::json{{"i", 2}}) };
        return { makeCall("emit", nlohmann::json{{"summary", "done"}}) };
    });
    sr.run("t_multi", "T");

    CHECK(a == 1);   // both parallel calls ran, not just the first
    CHECK(b == 1);
}
