// P2 of 05_SKILL_IMPROVE.md: thinking mode, the "missed tool call" nudge, and
// config validation. No network.
#include "doctest.h"

#include "client/deepseek.h"
#include "core/skill_runner.h"
#include "core/skill_config.h"
#include "core/config.h"
#include "platform/isystem_util.h"
#include "json.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

void ensureSysUtil() {
    static bool once = [] {
        SystemUtil::Init(CreateSystemUtil());
        return true;
    }();
    (void)once;
}
ToolCall makeCall(const std::string& name, const nlohmann::json& args) {
    ToolCall tc;
    tc.id = "call_" + name;
    tc.name = name;
    tc.arguments = args.dump();
    tc.index = 0;
    tc.valid = true;
    return tc;
}
ToolCall noCall() {
    ToolCall tc;
    tc.valid = false;
    return tc;
}
AppConfig testConfig() {
    AppConfig c;
    c.model = "deepseek-v4-flash";
    return c;
}

std::string writeSkill(const std::string& name, const std::string& json) {
    std::error_code ec;
    const std::string dir = getProjvDir() + "/skills/" + name;
    std::filesystem::create_directories(dir, ec);
    {
        std::ofstream f(dir + "/config.json");
        f << json;
    }
    {
        std::ofstream f(dir + "/p.md");
        f << "prompt";
    }
    std::filesystem::create_directories(dir + "/tools", ec);
    {
        std::ofstream f(dir + "/tools/emit.json");
        f << R"JSON({"name":"emit","description":"Report.","parameters":[{"name":"summary","type":"string","required":true}]})JSON";
    }
    return dir;
}

} // namespace

TEST_CASE("thinking: the request body only carries the field when it is off") {
    DeepSeekClient client;
    ChatRequest req;
    req.model = "deepseek-v4-flash";
    req.messages.push_back(Message::User("hi"));

    req.thinking = true;
    const std::string onBody = client.buildRequestBody(req);
    CHECK(onBody.find("\"thinking\"") == std::string::npos); // default: do not send
    CHECK(onBody.find("\"reasoning_effort\": \"max\"") != std::string::npos);

    req.thinking = false;
    const std::string offBody = client.buildRequestBody(req);
    auto j = nlohmann::json::parse(offBody);
    CHECK(j.contains("thinking"));
    CHECK(j["thinking"]["type"] == "disabled");
    CHECK_FALSE(j.contains("reasoning_effort")); // never send conflicting controls
}

TEST_CASE("thinking: reasoning_effort follows the official enum") {
    DeepSeekClient client;
    ChatRequest req;
    req.model = "deepseek-v4-flash";
    req.messages.push_back(Message::User("hi"));

    // Canonical values pass through, aliases are mapped the way the API maps them.
    CHECK(normalizeReasoningEffort("max") == "max");
    CHECK(normalizeReasoningEffort("HIGH") == "high");
    CHECK(normalizeReasoningEffort(" low ") == "low");
    CHECK(normalizeReasoningEffort("none") == "none");
    CHECK(normalizeReasoningEffort("minimal") == "low");
    CHECK(normalizeReasoningEffort("medium") == "high");
    CHECK(normalizeReasoningEffort("xhigh") == "high");
    CHECK(normalizeReasoningEffort("ultra") == "max");
    // Anything else is rejected so the caller can fall back to "high".
    CHECK(normalizeReasoningEffort("off").empty());
    CHECK(normalizeReasoningEffort("").empty());
    CHECK(normalizeReasoningEffort("highest").empty());

    // "none" is the documented "do not think" value: toggle, not an effort level.
    req.reasoningEffort = "none";
    auto j = nlohmann::json::parse(client.buildRequestBody(req));
    CHECK(j["thinking"]["type"] == "disabled");
    CHECK_FALSE(j.contains("reasoning_effort"));
}

TEST_CASE("thinking: role level overrides the skill level") {
    CHECK(SkillConfig::thinkingEnabled("on", ""));     // inherit on
    CHECK(!SkillConfig::thinkingEnabled("off", ""));   // inherit off
    CHECK(!SkillConfig::thinkingEnabled("on", "off")); // role wins
    CHECK(SkillConfig::thinkingEnabled("off", "on"));  // role wins
}

TEST_CASE("config: illegal values fail with the allowed set spelled out") {
    ensureSysUtil();
    SkillConfig cfg;
    std::string err;

    const std::string dir = writeSkill("cfg_bad_toolmode", R"JSON({
      "name": "cfg_bad_toolmode", "agents": [{"id":"a","prompt":"p.md"}],
      "tool_mode": "forced"
    })JSON");
    CHECK(!SkillConfig::loadFromJson(dir + "/config.json", cfg, err));
    CHECK(err.find("tool_mode") != std::string::npos);
    CHECK(err.find("auto") != std::string::npos);

    err.clear();
    const std::string dir2 = writeSkill("cfg_bad_thinking", R"JSON({
      "name": "cfg_bad_thinking", "agents": [{"id":"a","prompt":"p.md"}],
      "thinking": "maybe"
    })JSON");
    CHECK(!SkillConfig::loadFromJson(dir2 + "/config.json", cfg, err));
    CHECK(err.find("thinking") != std::string::npos);

    err.clear();
    const std::string dir3 = writeSkill("cfg_bad_shape", R"JSON({
      "name": "cfg_bad_shape", "agents": [{"id":"a","prompt":"p.md"}],
      "round_steps": [{"actions": [{"agent":"a","tool":"emit","shape":"voting"}]}]
    })JSON");
    CHECK(!SkillConfig::loadFromJson(dir3 + "/config.json", cfg, err));
    CHECK(err.find("shape") != std::string::npos);

    err.clear();
    const std::string dir4 = writeSkill("cfg_bad_agent", R"JSON({
      "name": "cfg_bad_agent", "agents": [{"id":"a","prompt":"p.md"}],
      "round_steps": [{"actions": [{"agent":"typo","tool":"emit"}]}]
    })JSON");
    CHECK(!SkillConfig::loadFromJson(dir4 + "/config.json", cfg, err));
    CHECK(err.find("unknown agent") != std::string::npos);
}

TEST_CASE("config: unknown keys become warnings, not failures") {
    ensureSysUtil();
    SkillConfig cfg;
    std::string err;

    // max_recursion was removed in P3a: old configs must still load.
    const std::string dir = writeSkill("cfg_warn", R"JSON({
      "name": "cfg_warn",
      "agents": [{"id":"a","prompt":"p.md","display":"A"}],
      "toolMode": "auto",
      "max_recursion": 3,
      "round_steps": [{"action": [], "actions": [{"agent":"a","tool":"emit"}]}]
    })JSON");
    CHECK(SkillConfig::loadFromJson(dir + "/config.json", cfg, err));

    bool sawTypo = false, sawLegacy = false, sawAgent = false;
    for (const auto& w : cfg.warnings) {
        if (w.find("'toolMode'") != std::string::npos &&
            w.find("did you mean 'tool_mode'") != std::string::npos)
            sawTypo = true;
        if (w.find("'max_recursion'") != std::string::npos) sawLegacy = true;
        if (w.find("agents.a.display") != std::string::npos) sawAgent = true;
    }
    CHECK(sawTypo);
    CHECK(sawLegacy);
    CHECK(sawAgent);
}

TEST_CASE("config: warnings are emitted as system events when the skill runs") {
    ensureSysUtil();
    writeSkill("cfg_emit_warn", R"JSON({
      "name": "cfg_emit_warn",
      "agents": [{"id":"a","prompt":"p.md"}],
      "toolMode": "auto",
      "max_rounds": 1,
      "round_steps": [{"actions": [{"agent":"a","tool":"emit","capture":"summary","emit":"message","message":"go"}]}]
    })JSON");

    std::vector<std::string> events;
    SkillRunner::Callbacks cbs;
    cbs.onEvent = [&events](const std::string& r, const std::string& k, const std::string& t) {
        events.push_back(r + "|" + k + "|" + t);
    };
    SkillRunner sr(testConfig(), nullptr, cbs, "", getProjvDir() + "/skills");
    sr.setResponder("a", [](const std::string&, const std::vector<Message>&) {
        return makeCall("emit", nlohmann::json{{"summary", "done"}});
    });
    sr.run("cfg_emit_warn", "T");

    bool sawWarning = false;
    for (const auto& e : events)
        if (e.rfind("system|system|[skill] config: unknown key 'toolMode'", 0) == 0)
            sawWarning = true;
    CHECK(sawWarning);
}

TEST_CASE("nudge: a prose-only reply triggers one 'call the tool' retry") {
    ensureSysUtil();
    writeSkill("nudge_test", R"JSON({
      "name": "nudge_test",
      "agents": [{"id":"a","prompt":"p.md"}],
      "tool_mode": "auto",
      "max_tool_iters": 3,
      "max_rounds": 1,
      "round_steps": [{"actions": [{"agent":"a","tool":"emit","capture":"summary","emit":"message","message":"go"}]}]
    })JSON");

    std::vector<std::vector<Message>> seen;
    int calls = 0;
    SkillRunner::Callbacks cbs;
    SkillRunner sr(testConfig(), nullptr, cbs, "", getProjvDir() + "/skills");
    sr.setResponder("a", [&calls](const std::string&, const std::vector<Message>&) {
        if (++calls == 1) return noCall(); // prose only
        return makeCall("emit", nlohmann::json{{"summary", "submitted"}});
    });
    sr.setResponderText("a", [&seen](const std::string&, const std::vector<Message>& m) {
        seen.push_back(m);
        return std::string("I think I am done here."); // narration, no tool call
    });
    sr.run("nudge_test", "T");

    CHECK(calls == 2); // nudged exactly once
    REQUIRE(!seen.empty());
    bool sawNudge = false;
    for (const auto& msg : seen.back())
        if (msg.role == "user" &&
            msg.content.find("Please call the 'emit' tool") != std::string::npos)
            sawNudge = true;
    CHECK(sawNudge);
}

TEST_CASE("nudge: a second prose-only reply ends the turn instead of looping") {
    ensureSysUtil();
    writeSkill("nudge_once", R"JSON({
      "name": "nudge_once",
      "agents": [{"id":"a","prompt":"p.md"}],
      "tool_mode": "auto",
      "max_tool_iters": 5,
      "max_rounds": 1,
      "round_steps": [{"actions": [{"agent":"a","tool":"emit","capture":"summary","emit":"message","message":"go"}]}]
    })JSON");

    int calls = 0;
    SkillRunner::Callbacks cbs;
    SkillRunner sr(testConfig(), nullptr, cbs, "", getProjvDir() + "/skills");
    sr.setResponder("a", [&calls](const std::string&, const std::vector<Message>&) {
        ++calls;
        return noCall(); // never calls the tool
    });
    sr.setResponderText("a", [](const std::string&, const std::vector<Message>&) {
        return std::string("still just talking");
    });
    sr.run("nudge_once", "T");

    CHECK(calls == 2); // one nudge, then the narration is returned as the result
}
