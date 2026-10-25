// P3a of 05_SKILL_IMPROVE.md: one reply fans out to several fixed workers in
// parallel, one layer only, results written back in call order. No API.
#include "doctest.h"

#include "core/skill_runner.h"
#include "core/config.h"
#include "platform/isystem_util.h"
#include "json.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

void ensureSysUtil() {
    static bool once = [] { SystemUtil::Init(CreateSystemUtil()); return true; }();
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
std::string lastUser(const std::vector<Message>& msgs) {
    for (auto it = msgs.rbegin(); it != msgs.rend(); ++it)
        if (it->role == "user") return it->content;
    return "";
}
AppConfig testConfig() { AppConfig c; c.model = "deepseek-v4-flash"; return c; }

std::string skillsRoot() { return getProjvDir() + "/skills"; }

std::string writeSwarm(const std::string& name, int workers, int maxParallel) {
    std::error_code ec;
    const std::string dir = getProjvDir() + "/skills/" + name;
    std::filesystem::create_directories(dir + "/tools", ec);

    std::string agents = R"({"id":"boss","display_name":"Boss","prompt":"p.md"})";
    for (int i = 1; i <= workers; ++i)
        agents += R"(,{"id":"w)" + std::to_string(i) + R"(","display_name":"W)" +
                  std::to_string(i) + R"(","prompt":"p.md"})";
    const std::string json =
        R"({"name":")" + name + R"(","tools":[],"agents":[)" + agents +
        R"(],"max_rounds":1,"converge":"none","dynamic_dispatch":true,)"
        R"("dispatch_tool":"speak","max_parallel":)" + std::to_string(maxParallel) +
        R"(,"round_steps":[{"actions":[{"agent":"boss","tool":"speak","capture":"text",)"
        R"("into":"boss","emit":"message","message":"Dispatch."}]}],"on_converge":[]})";
    { std::ofstream f(dir + "/config.json"); f << json; }
    { std::ofstream f(dir + "/p.md"); f << "prompt"; }
    { std::ofstream f(dir + "/tools/speak.json");
      f << R"JSON({"name":"speak","description":"Say.","parameters":[{"name":"text","type":"string","required":true}]})JSON"; }
    return dir;
}

// Fans one reply out to `workers` targets, then finishes on the next call.
void setFanOut(SkillRunner& sr, int workers, std::function<void()> beforeDone = {}) {
    auto calls = std::make_shared<int>(0);
    sr.setResponderMulti("boss", [workers, beforeDone, calls](const std::string&,
                                                             const std::vector<Message>&) {
        std::vector<ToolCall> out;
        if (++*calls > 1) {   // afterwards the driver submits and the turn ends
            out.push_back(makeCall("speak", {{"text", "done"}}));
            return out;
        }
        for (int i = 1; i <= workers; ++i) {
            ToolCall tc = makeCall("request_agent", {{"agent_id", "w" + std::to_string(i)},
                                                     {"message", "task" + std::to_string(i)}});
            tc.id = "ra" + std::to_string(i);
            out.push_back(tc);
        }
        if (beforeDone) beforeDone();
        return out;
    });
}

} // namespace

TEST_CASE("swarm: one reply fans out to four workers, all in flight together") {
    ensureSysUtil();
    writeSwarm("swarm_par", 4, 16);

    std::atomic<int> inFlight{0}, peak{0}, completed{0};
    auto worker = [&](const std::string& tag) {
        return [&, tag](const std::string&, const std::vector<Message>& m) {
            const int now = ++inFlight;
            int prev = peak.load();
            while (now > prev && !peak.compare_exchange_weak(prev, now)) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            --inFlight;
            ++completed;
            return makeCall("speak", nlohmann::json{{"text", tag + ":" + lastUser(m)}});
        };
    };

    SkillRunner::Callbacks cbs;
    SkillRunner sr(testConfig(), nullptr, cbs, "", skillsRoot());
    setFanOut(sr, 4);
    sr.setResponder("w1", worker("w1"));
    sr.setResponder("w2", worker("w2"));
    sr.setResponder("w3", worker("w3"));
    sr.setResponder("w4", worker("w4"));
    sr.run("swarm_par", "T");

    CHECK(completed.load() == 4);
    CHECK(peak.load() == 4);
}

TEST_CASE("swarm: max_parallel caps how many groups run at once") {
    ensureSysUtil();
    writeSwarm("swarm_cap", 4, 2);

    std::atomic<int> inFlight{0}, peak{0}, completed{0};
    auto worker = [&](const std::string& tag) {
        return [&, tag](const std::string&, const std::vector<Message>&) {
            const int now = ++inFlight;
            int prev = peak.load();
            while (now > prev && !peak.compare_exchange_weak(prev, now)) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            --inFlight;
            ++completed;
            return makeCall("speak", nlohmann::json{{"text", tag}});
        };
    };

    SkillRunner::Callbacks cbs;
    SkillRunner sr(testConfig(), nullptr, cbs, "", skillsRoot());
    setFanOut(sr, 4);
    for (int i = 1; i <= 4; ++i) sr.setResponder("w" + std::to_string(i), worker("w"));
    sr.run("swarm_cap", "T");

    CHECK(completed.load() == 4);
    CHECK(peak.load() == 2);
}

TEST_CASE("swarm: repeated dispatches to one worker stay serial and ordered") {
    ensureSysUtil();
    writeSwarm("swarm_serial", 1, 16);

    std::atomic<int> inFlight{0}, peak{0};
    std::vector<std::string> order;
    std::mutex orderMutex;

    SkillRunner::Callbacks cbs;
    SkillRunner sr(testConfig(), nullptr, cbs, "", skillsRoot());
    auto bossCalls = std::make_shared<int>(0);
    sr.setResponderMulti("boss", [bossCalls](const std::string&, const std::vector<Message>&) {
        std::vector<ToolCall> out;
        if (++*bossCalls > 1) {
            out.push_back(makeCall("speak", {{"text", "done"}}));
            return out;
        }
        for (int i = 1; i <= 3; ++i) {
            ToolCall tc = makeCall("request_agent", {{"agent_id", "w1"},
                                                     {"message", "job" + std::to_string(i)}});
            tc.id = "ra" + std::to_string(i);
            out.push_back(tc);
        }
        return out;
    });
    sr.setResponder("w1", [&](const std::string&, const std::vector<Message>& m) {
        const int now = ++inFlight;
        int prev = peak.load();
        while (now > prev && !peak.compare_exchange_weak(prev, now)) {}
        {
            std::lock_guard<std::mutex> lk(orderMutex);
            order.push_back(lastUser(m));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        --inFlight;
        return makeCall("speak", nlohmann::json{{"text", "ok"}});
    });
    sr.run("swarm_serial", "T");

    CHECK(peak.load() == 1);          // same target never overlaps
    REQUIRE(order.size() == 3);
    CHECK(order[0] == "job1");        // and keeps the request order
    CHECK(order[1] == "job2");
    CHECK(order[2] == "job3");
}

TEST_CASE("swarm: tool results come back in the original call order") {
    ensureSysUtil();
    writeSwarm("swarm_order", 4, 16);

    std::vector<std::string> toolIds, toolBodies;
    SkillRunner::Callbacks cbs;
    SkillRunner sr(testConfig(), nullptr, cbs, "", skillsRoot());

    // Boss: first reply fans out, second reply records what came back.
    int bossCalls = 0;
    sr.setResponderMulti("boss", [&](const std::string&, const std::vector<Message>& m) {
        std::vector<ToolCall> out;
        if (++bossCalls == 1) {
            for (int i = 1; i <= 4; ++i) {
                ToolCall tc = makeCall("request_agent", {{"agent_id", "w" + std::to_string(i)},
                                                         {"message", "job" + std::to_string(i)}});
                tc.id = "ra" + std::to_string(i);
                out.push_back(tc);
            }
            return out;
        }
        for (const auto& msg : m) {
            if (msg.role != "tool") continue;
            toolIds.push_back(msg.toolCallId);
            toolBodies.push_back(msg.content);
        }
        out.push_back(makeCall("speak", {{"text", "done"}}));
        return out;
    });
    // Later workers answer faster: completion order != call order on purpose.
    for (int i = 1; i <= 4; ++i) {
        const int delay = (5 - i) * 40;
        sr.setResponder("w" + std::to_string(i),
                        [i, delay](const std::string&, const std::vector<Message>&) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            return makeCall("speak", {{"text", "w" + std::to_string(i) + "-reply"}});
        });
    }
    sr.run("swarm_order", "T");

    REQUIRE(toolIds.size() == 4);
    CHECK(toolIds[0] == "ra1");
    CHECK(toolIds[1] == "ra2");
    CHECK(toolIds[2] == "ra3");
    CHECK(toolIds[3] == "ra4");
}

TEST_CASE("swarm: workers cannot dispatch, and drivers cannot be dispatched to") {
    ensureSysUtil();
    writeSwarm("swarm_layer", 2, 16);

    std::string selfDispatch;
    int w2Calls = 0;
    SkillRunner::Callbacks cbs;
    SkillRunner sr(testConfig(), nullptr, cbs, "", skillsRoot());
    int bossCalls = 0;
    sr.setResponderMulti("boss", [&](const std::string&, const std::vector<Message>& m) {
        std::vector<ToolCall> out;
        if (++bossCalls == 1) {
            out.push_back(makeCall("request_agent", {{"agent_id", "w1"}, {"message", "go"}}));
            out.back().id = "ra_w1";
            out.push_back(makeCall("request_agent", {{"agent_id", "boss"}, {"message", "me"}}));
            out.back().id = "ra_boss";
            return out;
        }
        for (const auto& msg : m)
            if (msg.role == "tool" && msg.toolCallId == "ra_boss") selfDispatch = msg.content;
        out.push_back(makeCall("speak", {{"text", "done"}}));
        return out;
    });
    // The worker tries to dispatch: it must not even see the tool.
    sr.setResponderMulti("w1", [&](const std::string&, const std::vector<Message>&) {
        std::vector<ToolCall> out;
        out.push_back(makeCall("request_agent", {{"agent_id", "w2"}, {"message", "help"}}));
        out.back().id = "ra_nested";
        out.push_back(makeCall("speak", {{"text", "w1 done"}}));
        out.back().id = "sp_1";
        return out;
    });
    sr.setResponder("w2", [&w2Calls](const std::string&, const std::vector<Message>&) {
        ++w2Calls;
        return makeCall("speak", {{"text", "w2 should never run"}});
    });
    sr.run("swarm_layer", "T");

    CHECK(selfDispatch.find("cannot dispatch to a driver agent") != std::string::npos);
    // w1's nested dispatch was refused, so w2 was never started.
    CHECK(w2Calls == 0);
}

TEST_CASE("swarm: a throwing worker is turned into an error result, not a crash") {
    ensureSysUtil();
    writeSwarm("swarm_throw", 2, 16);

    std::string seen;
    SkillRunner::Callbacks cbs;
    SkillRunner sr(testConfig(), nullptr, cbs, "", skillsRoot());

    int bossCalls = 0;
    sr.setResponderMulti("boss", [&](const std::string&, const std::vector<Message>& m) {
        std::vector<ToolCall> out;
        if (++bossCalls == 1) {
            out.push_back(makeCall("request_agent", {{"agent_id", "w1"}, {"message", "boom"}}));
            out.back().id = "ra_throw";
            out.push_back(makeCall("request_agent", {{"agent_id", "w2"}, {"message", "fine"}}));
            out.back().id = "ra_ok";
            return out;
        }
        for (const auto& msg : m)
            if (msg.role == "tool" && msg.toolCallId == "ra_throw") seen = msg.content;
        out.push_back(makeCall("speak", {{"text", "done"}}));
        return out;
    });
    sr.setResponder("w1", [](const std::string&, const std::vector<Message>&) -> ToolCall {
        throw std::runtime_error("responder exploded");
    });
    sr.setResponder("w2", [](const std::string&, const std::vector<Message>&) {
        return makeCall("speak", {{"text", "ok"}});
    });
    sr.run("swarm_throw", "T");

    CHECK(seen.find("[error]") != std::string::npos);
    CHECK(seen.find("responder exploded") != std::string::npos);
}

TEST_CASE("swarm: cancel before the run keeps every worker idle") {
    ensureSysUtil();
    writeSwarm("swarm_cancel", 4, 16);

    std::atomic<int> completed{0};
    SkillRunner::Callbacks cbs;
    SkillRunner sr(testConfig(), nullptr, cbs, "", skillsRoot());
    setFanOut(sr, 4);
    for (int i = 1; i <= 4; ++i)
        sr.setResponder("w" + std::to_string(i),
                        [&completed](const std::string&, const std::vector<Message>&) {
            ++completed;
            return makeCall("speak", {{"text", "ok"}});
        });

    sr.cancel();
    sr.run("swarm_cancel", "T");
    CHECK(completed.load() == 0);
}
