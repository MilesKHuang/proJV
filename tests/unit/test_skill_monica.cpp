// monica integration test: the example skill (repo ./skills/monica) must be
// discoverable, its config valid, and the engine must fan one supervisor reply
// out to several workers at once until stop_when fires. Scripted, no network.
#include "doctest.h"

#include "core/skill_runner.h"
#include "core/config.h"
#include "platform/isystem_util.h"
#include "json.hpp"

#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
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
ToolCall emitCall(const std::string& s) {
    return makeCall("emit", nlohmann::json{{"summary", s}});
}
AppConfig testConfig() {
    AppConfig c;
    c.model = "deepseek-v4-flash";
    return c;
}
std::string readFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), {});
}
std::string lastUser(const std::vector<Message>& msgs) {
    for (auto it = msgs.rbegin(); it != msgs.rend(); ++it)
        if (it->role == "user") return it->content;
    return "";
}

} // namespace

TEST_CASE("monica: config is the 1 supervisor + 10 worker shape") {
    ensureSysUtil();
    const std::string root = PROJV_EXAMPLE_SKILLS_DIR;

    bool found = false;
    for (auto& n : SkillRunner::listSkills(root))
        if (n == "monica") found = true;
    CHECK(found);

    auto cfg = nlohmann::json::parse(readFile(root + "/monica/config.json"));
    CHECK(cfg.value("dynamic_dispatch", false) == true);
    CHECK(cfg.value("tool_mode", std::string("")) == "auto");
    CHECK(cfg.value("max_parallel", 0) == 16);
    CHECK(cfg.value("max_rounds", 0) == 10);
    // Batches are 3-4 files, so a worker needs more than the old 60 calls.
    CHECK(cfg.value("max_tool_iters", 0) == 90);
    CHECK(!cfg.contains("max_recursion"));
    CHECK(cfg["stop_when"].value("slot", std::string("")) == "supervisor");

    // No shell for workers: the process runner is a single serial resource.
    const auto tools = cfg["tools"].get<std::vector<std::string>>();
    bool hasShell = false, hasFormat = false, hasListDir = false;
    for (const auto& t : tools) {
        if (t == "exec_shell") hasShell = true;
        if (t == "format_file") hasFormat = true;
        if (t == "list_dir") hasListDir = true;
    }
    CHECK(!hasShell);
    CHECK(hasFormat);
    CHECK(hasListDir);

    CHECK(cfg["agents"].size() == 11);
    CHECK(cfg["agents"][0].value("id", std::string("")) == "supervisor");
    for (size_t i = 1; i < cfg["agents"].size(); ++i) {
        CHECK(cfg["agents"][i].value("prompt", std::string("")) == "worker.md");
        CHECK(cfg["agents"][i].value("thinking", std::string("")) == "off");
    }
}

TEST_CASE("monica: one supervisor reply dispatches to several workers, then DONE") {
    ensureSysUtil();
    const std::string root = PROJV_EXAMPLE_SKILLS_DIR;

    std::atomic<int> inFlight{0}, peak{0};
    std::vector<std::string> workerMessages;
    std::mutex mtx;
    std::vector<std::string> events;

    SkillRunner::Callbacks cbs;
    cbs.onEvent = [&events](const std::string& r, const std::string& k, const std::string& t) {
        events.push_back(r + "|" + k + "|" + t);
    };

    int bossCalls = 0;
    SkillRunner sr(testConfig(), nullptr, cbs, "", root);
    sr.setResponderMulti("supervisor", [&bossCalls](const std::string&,
                                                    const std::vector<Message>&) {
        std::vector<ToolCall> out;
        if (++bossCalls > 1) { // after the batch: submit
            out.push_back(emitCall("DONE"));
            return out;
        }
        for (int i = 1; i <= 4; ++i) { // four batches, one reply
            ToolCall tc = makeCall("request_agent",
                                   {{"agent_id", "worker_" + std::to_string(i)},
                                    {"message", "Files:\n- src/f" + std::to_string(i) + ".cpp"}});
            tc.id = "ra" + std::to_string(i);
            out.push_back(tc);
        }
        return out;
    });

    // Every worker is scripted: a missing responder would hit the real API.
    for (int w = 1; w <= 10; ++w) {
        sr.setResponder("worker_" + std::to_string(w),
                        [&](const std::string&, const std::vector<Message>& m) {
                            const int now = ++inFlight;
                            int prev = peak.load();
                            while (now > prev && !peak.compare_exchange_weak(prev, now)) {
                            }
                            {
                                std::lock_guard<std::mutex> lk(mtx);
                                workerMessages.push_back(lastUser(m));
                            }
                            std::this_thread::sleep_for(std::chrono::milliseconds(60));
                            --inFlight;
                            return emitCall("ok");
                        });
    }

    sr.run("monica", "D:/ws/projv");

    CHECK(workerMessages.size() == 4); // exactly the four dispatched batches
    CHECK(peak.load() >= 2);           // they really overlapped
    CHECK(sr.lastRounds() == 1);       // stop_when fired in round 1

    bool done = false;
    for (auto& e : events)
        if (e == "Supervisor|message|DONE") done = true;
    CHECK(done);
}

TEST_CASE("skill: a 'DONE' answer in the final step is retried, then reported") {
    // Exactly what happened with monica: the supervisor answered DONE in the
    // on_converge step too, so the user got a "finished" with no deliverable.
    ensureSysUtil();
    const std::string root = PROJV_EXAMPLE_SKILLS_DIR;

    std::vector<std::string> events;
    SkillRunner::Callbacks cbs;
    cbs.onEvent = [&events](const std::string& r, const std::string& k, const std::string& t) {
        events.push_back(r + "|" + k + "|" + t);
    };

    int convergeCalls = 0;
    SkillRunner sr(testConfig(), nullptr, cbs, "", root);
    sr.setResponderMulti("supervisor",
                         [&convergeCalls](const std::string&, const std::vector<Message>&) {
                             ++convergeCalls;
                             std::vector<ToolCall> out;
                             out.push_back(emitCall("DONE"));
                             return out;
                         });
    for (int w = 1; w <= 10; ++w)
        sr.setResponder(
            "worker_" + std::to_string(w),
            [](const std::string&, const std::vector<Message>&) { return emitCall("ok"); });

    sr.run("monica", "T");

    // on_converge ran twice: the first answer was trivial, so it retried once.
    bool retried = false, warned = false;
    for (const auto& e : events) {
        if (e.find("retrying once") != std::string::npos) retried = true;
        if (e.find("no deliverable was produced") != std::string::npos) warned = true;
    }
    CHECK(retried);
    CHECK(warned); // and it says so instead of returning silently
}

TEST_CASE("skill: a real report in the final step is not retried") {
    ensureSysUtil();
    const std::string root = PROJV_EXAMPLE_SKILLS_DIR;

    std::vector<std::string> events;
    SkillRunner::Callbacks cbs;
    cbs.onEvent = [&events](const std::string& r, const std::string& k, const std::string& t) {
        events.push_back(r + "|" + k + "|" + t);
    };

    int calls = 0;
    SkillRunner sr(testConfig(), nullptr, cbs, "", root);
    sr.setResponderMulti("supervisor", [&calls](const std::string&, const std::vector<Message>&) {
        ++calls;
        std::vector<ToolCall> out;
        out.push_back(emitCall("## Report\n\n- src/a.cpp: formatted, 3 full-width chars "
                               "replaced\n- src/b.cpp: dead code at line 42 (unused symbol)"));
        return out;
    });
    for (int w = 1; w <= 10; ++w)
        sr.setResponder(
            "worker_" + std::to_string(w),
            [](const std::string&, const std::vector<Message>&) { return emitCall("ok"); });

    sr.run("monica", "T");

    for (const auto& e : events) {
        CHECK(e.find("retrying once") == std::string::npos);
        CHECK(e.find("no deliverable was produced") == std::string::npos);
    }
}

TEST_CASE("skill: a stale cancel flag is reported, not silently swallowed") {
    // The real failure: ESC / F3 / F5 / /clear latch ToolRegistry's cancel flag, and
    // a user-invoked skill never calls Agent::newTurn() to clear it. Every step was
    // skipped and run() returned with zero events -- the user saw nothing at all.
    ensureSysUtil();
    const std::string root = PROJV_EXAMPLE_SKILLS_DIR;

    std::vector<std::string> events;
    SkillRunner::Callbacks cbs;
    cbs.onEvent = [&events](const std::string& r, const std::string& k, const std::string& t) {
        events.push_back(r + "|" + k + "|" + t);
    };

    SkillRunner sr(testConfig(), nullptr, cbs, "", root);
    sr.setCancelProbe([] { return true; }); // as if the flag were left behind
    sr.run("monica", "D:/ws/projv");

    REQUIRE(events.size() == 1);
    CHECK(events[0].find("cancelled before the first round") != std::string::npos);
}

TEST_CASE("skill: a clean start runs with no probe hits") {
    ensureSysUtil();
    const std::string root = PROJV_EXAMPLE_SKILLS_DIR;

    std::vector<std::string> events;
    SkillRunner::Callbacks cbs;
    cbs.onEvent = [&events](const std::string& r, const std::string& k, const std::string& t) {
        events.push_back(r + "|" + k + "|" + t);
    };

    SkillRunner sr(testConfig(), nullptr, cbs, "", root);
    sr.setCancelProbe([] { return false; });
    sr.setResponderMulti("supervisor", [](const std::string&, const std::vector<Message>&) {
        std::vector<ToolCall> out;
        out.push_back(emitCall("DONE"));
        return out;
    });
    sr.run("monica", "D:/ws/projv");

    CHECK(events.size() >= 1);
    bool cancelled = false;
    for (auto& e : events)
        if (e.find("cancelled before") != std::string::npos) cancelled = true;
    CHECK_FALSE(cancelled);
}
