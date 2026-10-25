// Engine-level contract fixes (05 follow-up, after the first monica run):
//  - a turn never returns an empty string;
//  - submit_reserve is opt-in and, at its default of 0, changes nothing.
#include "doctest.h"

#include "core/skill_runner.h"
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
    std::filesystem::create_directories(dir + "/tools", ec);
    {
        std::ofstream f(dir + "/config.json");
        f << json;
    }
    {
        std::ofstream f(dir + "/p.md");
        f << "prompt";
    }
    {
        std::ofstream f(dir + "/tools/emit.json");
        f << R"JSON({"name":"emit","description":"Report.","parameters":[{"name":"summary","type":"string","required":true}]})JSON";
    }
    {
        std::ofstream f(dir + "/tools/speak.json");
        f << R"JSON({"name":"speak","description":"Say.","parameters":[{"name":"text","type":"string","required":true}]})JSON";
    }
    return dir;
}

// One agent, one action, verb = emit unless the caller overrides it.
std::string skillJson(const std::string& name, int maxIters, int reserve,
                      const std::string& verb = "emit") {
    return R"({"name":")" + name +
           R"(","tools":[],"agents":[{"id":"a","prompt":"p.md"}],)"
           R"("tool_mode":"auto","max_tool_iters":)" +
           std::to_string(maxIters) + R"(,"submit_reserve":)" + std::to_string(reserve) +
           R"(,"max_rounds":1,"converge":"none","round_steps":[{"actions":[)"
           R"({"agent":"a","tool":")" +
           verb +
           R"(","capture":"summary","into":"out",)"
           R"("emit":"message","message":"go"}]}],"on_converge":[]})";
}

bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

} // namespace

TEST_CASE("contract: a turn whose budget runs out returns a labelled marker") {
    ensureSysUtil();
    writeSkill("budget_out", skillJson("budget_out", 2, 0));

    std::string slot;
    SkillRunner::Callbacks cbs;
    cbs.onEvent = [&slot](const std::string&, const std::string& kind, const std::string& t) {
        if (kind == "message") slot = t;
    };
    SkillRunner sr(testConfig(), nullptr, cbs, "", getProjvDir() + "/skills");
    // Keeps calling a non-verb tool until the budget dies: the monica failure
    // shape. Previously this returned "" and the batch looked like it vanished.
    sr.setResponder("a", [](const std::string&, const std::vector<Message>&) {
        return makeCall("speak", {{"text", "still exploring"}});
    });
    sr.run("budget_out", "T");

    CHECK(has(slot, "[no report]"));
    CHECK(has(slot, "'a'"));    // names the role
    CHECK(has(slot, "budget")); // says why
    CHECK(has(slot, "'emit'")); // and which tool it wanted
}

TEST_CASE("contract: a response with no text and no tool call is labelled too") {
    ensureSysUtil();
    writeSkill("budget_silent", skillJson("budget_silent", 1, 0));

    std::string slot;
    SkillRunner::Callbacks cbs;
    cbs.onEvent = [&slot](const std::string&, const std::string& kind, const std::string& t) {
        if (kind == "message") slot = t;
    };
    SkillRunner sr(testConfig(), nullptr, cbs, "", getProjvDir() + "/skills");
    sr.setResponder("a", [](const std::string&, const std::vector<Message>&) { return noCall(); });
    sr.run("budget_silent", "T");

    CHECK(has(slot, "[no report]"));
    CHECK(has(slot, "'a'"));
    CHECK(has(slot, "no text"));
}

TEST_CASE("contract: a non-monica skill gets the same marker, with its own verb") {
    ensureSysUtil();
    // bigbang-shaped: 3 iterations, verb "speak" instead of "emit".
    writeSkill("budget_bb", skillJson("budget_bb", 3, 0, "speak"));

    std::string slot;
    SkillRunner::Callbacks cbs;
    cbs.onEvent = [&slot](const std::string&, const std::string& kind, const std::string& t) {
        if (kind == "message") slot = t;
    };
    SkillRunner sr(testConfig(), nullptr, cbs, "", getProjvDir() + "/skills");
    sr.setResponder("a", [](const std::string&, const std::vector<Message>&) {
        return makeCall("emit", {{"summary", "wrong tool"}}); // never calls "speak"
    });
    sr.run("budget_bb", "T");

    CHECK(has(slot, "[no report]"));
    CHECK(has(slot, "'speak'")); // the marker follows the skill's verb, not monica's
}

TEST_CASE("submit_reserve: 0 is the default and injects nothing") {
    ensureSysUtil();
    writeSkill("budget_off", skillJson("budget_off", 5, 0));

    std::vector<std::vector<Message>> seen;
    int calls = 0;
    SkillRunner::Callbacks cbs;
    SkillRunner sr(testConfig(), nullptr, cbs, "", getProjvDir() + "/skills");
    sr.setResponder("a", [&calls](const std::string&, const std::vector<Message>&) {
        ++calls;
        return makeCall("speak", {{"text", "still exploring"}});
    });
    sr.setResponderText("a", [&seen](const std::string&, const std::vector<Message>& m) {
        seen.push_back(m);
        return std::string("thinking out loud");
    });
    sr.run("budget_off", "T");

    CHECK(calls == 5); // the whole budget, exactly as before
    bool sawHint = false;
    for (const auto& req : seen)
        for (const auto& msg : req)
            if (has(msg.content, "Tool budget nearly exhausted")) sawHint = true;
    CHECK(!sawHint);
}

TEST_CASE("submit_reserve: 2 tells the agent to submit once, near the end") {
    ensureSysUtil();
    writeSkill("budget_on", skillJson("budget_on", 5, 2));

    std::vector<std::vector<Message>> seen;
    SkillRunner::Callbacks cbs;
    SkillRunner sr(testConfig(), nullptr, cbs, "", getProjvDir() + "/skills");
    sr.setResponder("a", [](const std::string&, const std::vector<Message>&) {
        return makeCall("speak", {{"text", "still exploring"}});
    });
    sr.setResponderText("a", [&seen](const std::string&, const std::vector<Message>& m) {
        seen.push_back(m);
        return std::string("thinking out loud");
    });
    sr.run("budget_on", "T");

    // The hint stays in the session, so count it in the final request: it must
    // appear exactly once there (injected once, not repeated every iteration).
    int hints = 0;
    if (!seen.empty())
        for (const auto& msg : seen.back())
            if (has(msg.content, "Tool budget nearly exhausted")) {
                ++hints;
                CHECK(has(msg.content, "'emit'"));
            }
    CHECK(hints == 1);

    // And it must not be there before the reserve window opens.
    size_t firstSeen = 0;
    for (size_t i = 0; i < seen.size(); ++i) {
        bool hit = false;
        for (const auto& msg : seen[i])
            if (has(msg.content, "Tool budget nearly exhausted")) hit = true;
        if (hit) {
            firstSeen = i;
            break;
        }
    }
    CHECK(firstSeen == 3); // 5 iterations, reserve 2 -> 4th request (index 3)
}

TEST_CASE("submit_reserve: must be smaller than max_tool_iters") {
    ensureSysUtil();
    writeSkill("budget_bad", skillJson("budget_bad", 3, 3));
    SkillConfig cfg;
    std::string err;
    CHECK(!SkillConfig::loadFromJson(getProjvDir() + "/skills/budget_bad/config.json", cfg, err));
    CHECK(has(err, "submit_reserve"));
}
