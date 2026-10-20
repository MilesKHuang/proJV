// Tests for the skill trigger chain (doc 02):
//   - "/<skill> <topic>" resolution rules (skill_trigger::parse)
//   - SkillRunner::setCancelProbe: an external cancel source stops the run
//   - the run_skill invariant: no events -> the agent still gets lastDoc()
#include "doctest.h"

#include "core/skill_runner.h"
#include "core/skill_trigger.h"
#include "core/config.h"
#include "platform/isystem_util.h"
#include "json.hpp"

#include <string>
#include <vector>

namespace {

void ensureSysUtil() {
    static bool once = [] { SystemUtil::Init(CreateSystemUtil()); return true; }();
    (void)once;
}

AppConfig testConfig() { AppConfig c; c.model = "deepseek-v4-flash"; return c; }

ToolCall makeCall(const std::string& name, const nlohmann::json& args) {
    ToolCall tc; tc.id = "call_" + name; tc.name = name;
    tc.arguments = args.dump(); tc.index = 0; tc.valid = true; return tc;
}

std::vector<std::string> installed() { return {"bigbang", "monica"}; }

// Scripted bigbang run: every role answers in one round and converges, so the
// document tool fires and lastDoc() is set. No API, no key needed.
void scriptBigbang(SkillRunner& sr, const std::string& doc) {
    auto responder = [doc](const std::string& tool, const std::vector<Message>&) {
        if (tool == "bigbang_turn")
            return makeCall("bigbang_turn", nlohmann::json{{"statement", "S"}});
        if (tool == "bigbang_vote") {
            nlohmann::json v;
            v["agree"] = true;
            v["agreed_points"] = nlohmann::json::array();
            v["concerns"] = nlohmann::json::array();
            v["suggested_tweak"] = "none";
            return makeCall("bigbang_vote", v);
        }
        return makeCall("write_bigbang_doc", nlohmann::json{{"doc_markdown", doc}});
    };
    sr.setResponder("sheldon", responder);
    sr.setResponder("penny", responder);
    sr.setResponder("leonard", responder);
}

// Minimal single-role script for the cancel-probe test (never reached).
void scriptSheldonOnly(SkillRunner& sr) {
    sr.setResponder("sheldon", [](const std::string&, const std::vector<Message>&) {
        return makeCall("bigbang_turn", nlohmann::json{{"statement", "S"}});
    });
}

} // namespace

TEST_CASE("trigger: /<installed> <topic> fires") {
    const auto r = skill_trigger::parse("/monica D:\\ws\\proj", installed());
    CHECK(r.kind == skill_trigger::Result::Kind::Trigger);
    CHECK(r.name == "monica");
    CHECK(r.topic == "D:\\ws\\proj");
}

TEST_CASE("trigger: case-insensitive on the skill name only") {
    const auto r = skill_trigger::parse("/Monica Tidy Up", installed());
    CHECK(r.kind == skill_trigger::Result::Kind::Trigger);
    CHECK(r.name == "monica");
    CHECK(r.topic == "Tidy Up");
}

TEST_CASE("trigger: /skill and /skill list list the installed skills") {
    CHECK(skill_trigger::parse("/skill", installed()).kind ==
          skill_trigger::Result::Kind::List);
    CHECK(skill_trigger::parse("/skill list", installed()).kind ==
          skill_trigger::Result::Kind::List);
    CHECK(skill_trigger::parse("/skills", installed()).kind ==
          skill_trigger::Result::Kind::List);
}

TEST_CASE("trigger: unknown names pass through as ordinary messages") {
    // The whole point of the "/" domain: nothing is ever intercepted with an
    // error, so natural text and paths keep working.
    const char* lines[] = {
        "/mica hey",                 // typo / unknown skill
        "/usr/bin/env python",       // absolute path
        "//just a comment",          // C++ comment
        "./relative/path.txt",       // relative path
        "/clear",                    // reserved backend command
        "/help",
        "/workspace",
        "/compress",
        "no slash at all",
        "/"                          // bare slash
    };
    for (const char* line : lines) {
        const auto r = skill_trigger::parse(line, installed());
        CHECK_MESSAGE(r.kind == skill_trigger::Result::Kind::None, line);
    }
}

TEST_CASE("trigger: reserved commands win over a shadowing skill folder") {
    const std::vector<std::string> shadowed = {"clear", "help"};
    CHECK(skill_trigger::parse("/clear", shadowed).kind ==
          skill_trigger::Result::Kind::None);
    CHECK(skill_trigger::parse("/help", shadowed).kind ==
          skill_trigger::Result::Kind::None);
    CHECK(skill_trigger::isReservedCommand("compress"));
    CHECK_FALSE(skill_trigger::isReservedCommand("monica"));
}

TEST_CASE("trigger: empty topic is reported, not guessed") {
    const auto r = skill_trigger::parse("/monica", installed());
    CHECK(r.kind == skill_trigger::Result::Kind::Trigger);
    CHECK(r.topic.empty());
}

TEST_CASE("cancel probe: an external flag stops the run at the first boundary") {
    ensureSysUtil();

    SkillRunner::Callbacks cbs;
    std::string progress;
    cbs.onProgress = [&progress](const std::string& s) { progress = s; };

    SkillRunner sr(testConfig(), nullptr, cbs, "", PROJV_EXAMPLE_SKILLS_DIR);
    // If the probe is ignored, the scripted responders would run to completion.
    scriptSheldonOnly(sr);
    sr.setCancelProbe([]() { return true; });
    sr.run("bigbang", "T");

    CHECK(sr.lastRounds() == 0);          // nothing ran
    CHECK_FALSE(sr.converged());
    CHECK(progress == "Cancelled");
}

TEST_CASE("cancel probe: a false probe lets the run proceed") {
    ensureSysUtil();

    SkillRunner::Callbacks cbs;
    std::string progress;
    cbs.onProgress = [&progress](const std::string& s) { progress = s; };

    SkillRunner sr(testConfig(), nullptr, cbs, "", PROJV_EXAMPLE_SKILLS_DIR);
    scriptBigbang(sr, "# DOC");
    int probeCalls = 0;
    sr.setCancelProbe([&probeCalls]() { ++probeCalls; return false; });
    sr.run("bigbang", "T");

    CHECK(probeCalls > 0);
    CHECK(sr.lastDoc() == "# DOC");
}

TEST_CASE("run_skill invariant: no onEvent still yields the final document") {
    ensureSysUtil();

    std::vector<std::string> events;
    SkillRunner::Callbacks cbs;
    // onEvent deliberately empty -- this is exactly what the run_skill tool
    // executor does so skill chatter never enters the main session.
    cbs.onEvent = [&events](const std::string& role, const std::string& kind,
                            const std::string& text) {
        events.push_back(role + "|" + kind + "|" + text);
    };

    SkillRunner sr(testConfig(), nullptr, cbs, "", PROJV_EXAMPLE_SKILLS_DIR);
    scriptBigbang(sr, "# EXEC DOC");
    sr.run("bigbang", "T");

    CHECK(sr.lastDoc() == "# EXEC DOC");
    CHECK(sr.converged());
}
