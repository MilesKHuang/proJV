// monica integration test: the example skill (repo ./skills/monica) must be
// discoverable, its config valid, and the engine must run the supervisor's
// multi-round request_agent dispatch until stop_when fires. Scripted responders.
#include "doctest.h"

#include "core/skill_runner.h"
#include "core/config.h"
#include "platform/isystem_util.h"
#include "json.hpp"

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
ToolCall emitCall(const std::string& s) { return makeCall("emit", nlohmann::json{{"summary", s}}); }
AppConfig testConfig() { AppConfig c; c.model = "deepseek-v4-flash"; return c; }
std::string readFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), {});
}

} // namespace

TEST_CASE("monica: example skill runs a supervisor dispatch loop to stop_when") {
    ensureSysUtil();
    std::string root = PROJV_EXAMPLE_SKILLS_DIR;

    // discoverable + valid config
    bool found = false;
    for (auto& n : SkillRunner::listSkills(root)) if (n == "monica") found = true;
    CHECK(found);
    auto cfg = nlohmann::json::parse(readFile(root + "/monica/config.json"));
    CHECK(cfg.value("dynamic_dispatch", false) == true);
    CHECK(cfg["stop_when"].value("slot", std::string("")) == "supervisor");

    // supervisor dispatch loop: round 1 dispatch formatter, then working;
    // round 2 -> DONE -> stop_when fires.
    std::vector<std::string> fmt;
    std::vector<std::string> events;
    int sc = 0;
    auto supervisor = [&sc](const std::string&, const std::vector<Message>&) {
        int n = ++sc;
        if (n == 1) return makeCall("request_agent", nlohmann::json{{"agent_id", "formatter"}, {"message", "format src/"}});
        if (n == 2) return emitCall("working");
        return emitCall("DONE");
    };
    auto formatter = [&fmt](const std::string&, const std::vector<Message>& m) {
        for (auto it = m.rbegin(); it != m.rend(); ++it)
            if (it->role == "user") { fmt.push_back(it->content); break; }
        return emitCall("formatted 3 files");
    };
    auto noop = [](const std::string&, const std::vector<Message>&) { return emitCall("ok"); };

    SkillRunner::Callbacks cbs;
    cbs.onEvent = [&events](const std::string& r, const std::string& k, const std::string& t) {
        events.push_back(r + "|" + k + "|" + t);
    };
    SkillRunner sr(testConfig(), nullptr, cbs, "", root);
    sr.setResponder("supervisor", supervisor);
    sr.setResponder("formatter", formatter);
    sr.setResponder("translator", noop);
    sr.setResponder("auditor", noop);
    sr.run("monica", "D:/ws/projv");

    CHECK(fmt.size() == 1);                       // formatter was dispatched
    if (fmt.size() == 1) CHECK(fmt[0] == "format src/");
    CHECK(sr.lastRounds() == 2);                  // stopped by stop_when, not max_rounds
    bool done = false;
    for (auto& e : events) if (e == "Supervisor|message|DONE") done = true;
    CHECK(done);
}
