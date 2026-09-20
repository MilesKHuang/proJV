// Swarm feasibility probe: a manager dispatches to two workers via the
// request_agent interceptor (dynamic_dispatch). If the workers' responders
// fire, dispatch worked. Scripted responders, no API.
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
    static bool once = [] { SystemUtil::Init(CreateSystemUtil()); return true; }();
    (void)once;
}
ToolCall makeCall(const std::string& name, const nlohmann::json& args) {
    ToolCall tc; tc.id = "call_" + name; tc.name = name;
    tc.arguments = args.dump(); tc.index = 0; tc.valid = true; return tc;
}
std::string lastUser(const std::vector<Message>& msgs) {
    for (auto it = msgs.rbegin(); it != msgs.rend(); ++it)
        if (it->role == "user") return it->content;
    return "";
}
AppConfig testConfig() { AppConfig c; c.model = "deepseek-v4-flash"; return c; }

const char* SWARM_JSON = R"JSON({
  "name": "swarm_test",
  "description": "swarm probe",
  "tools": [],
  "agents": [
    {"id": "mgr", "display_name": "Manager", "prompt": "mgr.md"},
    {"id": "w1", "display_name": "Worker1", "prompt": "w1.md"},
    {"id": "w2", "display_name": "Worker2", "prompt": "w2.md"}
  ],
  "max_rounds": 1,
  "converge": "none",
  "dynamic_dispatch": true,
  "max_recursion": 2,
  "dispatch_tool": "speak",
  "round_steps": [
    {"actions": [
      {"agent": "mgr", "tool": "speak", "capture": "text", "into": "mgr", "emit": "message",
       "message": "Dispatch work to workers."}
    ]}
  ],
  "on_converge": []
}
)JSON";

} // namespace

TEST_CASE("swarm: manager dispatches to workers via request_agent") {
    ensureSysUtil();
    std::error_code ec;
    std::string sdir = getProjvDir() + "/skills/swarm_test";
    std::filesystem::create_directories(sdir, ec);
    { std::ofstream f(sdir + "/config.json"); f << SWARM_JSON; }
    std::filesystem::create_directories(sdir + "/tools", ec);
    {
        std::ofstream f(sdir + "/tools/speak.json");
        f << R"JSON({"name":"speak","description":"Say something.","parameters":[{"name":"text","type":"string","required":true}]})JSON";
    }

    std::vector<std::string> w1, w2;
    int mc = 0;
    auto mgr = [&mc](const std::string&, const std::vector<Message>&) {
        int n = ++mc;
        if (n == 1) return makeCall("request_agent", nlohmann::json{{"agent_id", "w1"}, {"message", "clean f1"}});
        if (n == 2) return makeCall("request_agent", nlohmann::json{{"agent_id", "w2"}, {"message", "clean f2"}});
        return makeCall("speak", nlohmann::json{{"text", "all done"}});
    };
    auto wk = [](std::vector<std::string>* sink, const char* tag) {
        return [sink, tag](const std::string&, const std::vector<Message>& m) {
            sink->push_back(lastUser(m));
            return makeCall("speak", nlohmann::json{{"text", std::string(tag) + " ok"}});
        };
    };

    SkillRunner::Callbacks cbs;
    SkillRunner sr(testConfig(), nullptr, cbs);
    sr.setResponder("mgr", mgr);
    sr.setResponder("w1", wk(&w1, "w1"));
    sr.setResponder("w2", wk(&w2, "w2"));
    sr.run("swarm_test", "T");

    CHECK(w1.size() == 1);
    CHECK(w2.size() == 1);
    if (w1.size() == 1) CHECK(w1[0] == "clean f1");
    if (w2.size() == 1) CHECK(w2[0] == "clean f2");
}
