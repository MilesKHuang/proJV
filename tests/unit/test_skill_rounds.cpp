// proJV -- a silent round must still show up in the chat.
//
// The bug: monica ran ten rounds; in nine of them the supervisor only dispatched
// (tool calls, no prose), so mergeAction had an empty string and emitted nothing.
// The chat showed one "DONE" and the final report -- ten rounds looked like one, and
// nothing told the user work was happening. The round count is the only progress
// signal a long skill gives.
#include "doctest/doctest.h"
#include "core/config.h"
#include "core/skill_runner.h"
#include "platform/isystem_util.h"

#include <string>
#include <vector>

namespace {

void ensureSys() {
    static bool done = false;
    if (!done) {
        SystemUtil::Init(CreateSystemUtil());
        done = true;
    }
}

AppConfig testCfg() {
    AppConfig c;
    c.apiKey = "test";
    c.model = "deepseek-v4-flash";
    c.baseUrl = "http://127.0.0.1:1";
    c.maxTokens = 1024;
    return c;
}

ToolCall mkCall(const std::string& name, const std::string& args) {
    ToolCall tc;
    tc.id = "call_" + name;
    tc.name = name;
    tc.arguments = args;
    return tc;
}

ToolCall emitCall(const std::string& text) {
    return mkCall("emit", std::string("{\"text\":\"") + text + "\"}");
}

ToolCall dispatchCall(const std::string& agent, const std::string& msg) {
    return mkCall("request_agent",
                  std::string("{\"agent_id\":\"") + agent + "\",\"message\":\"" + msg + "\"}");
}

bool anyEventContains(const std::vector<std::string>& events, const std::string& needle) {
    for (const auto& e : events)
        if (e.find(needle) != std::string::npos) return true;
    return false;
}

} // namespace

TEST_CASE("skill: a dispatch-only round still reports its round number") {
    ensureSys();
    std::vector<std::string> events;
    SkillRunner::Callbacks cbs;
    cbs.onEvent = [&events](const std::string& r, const std::string& k, const std::string& t) {
        events.push_back(r + "|" + k + "|" + t);
    };
    SkillRunner sr(testCfg(), nullptr, cbs, "", PROJV_EXAMPLE_SKILLS_DIR);
    int calls = 0;
    sr.setResponderMulti("supervisor", [&calls](const std::string&, const std::vector<Message>&) {
        ++calls;
        std::vector<ToolCall> out;
        if (calls == 1) {
            // Round 1: pure dispatch, no prose at all -- the real monica case.
            out.push_back(dispatchCall("worker_1", "format src/a.cpp"));
        } else {
            out.push_back(emitCall("DONE"));
        }
        return out;
    });
    for (int w = 1; w <= 10; ++w)
        sr.setResponder(
            "worker_" + std::to_string(w),
            [](const std::string&, const std::vector<Message>&) { return emitCall("ok"); });

    sr.run("monica", "T");

    CHECK(anyEventContains(events, "Round 1/"));
}

TEST_CASE("skill: a round WITH narration still gets its round line, once") {
    ensureSys();
    std::vector<std::string> events;
    SkillRunner::Callbacks cbs;
    cbs.onEvent = [&events](const std::string& r, const std::string& k, const std::string& t) {
        events.push_back(r + "|" + k + "|" + t);
    };

    SkillRunner sr(testCfg(), nullptr, cbs, "", PROJV_EXAMPLE_SKILLS_DIR);
    int calls = 0;
    sr.setResponderMulti("supervisor", [&calls](const std::string&, const std::vector<Message>&) {
        ++calls;
        std::vector<ToolCall> out;
        out.push_back(emitCall(calls >= 2 ? "DONE" : "dispatched 28 files to workers"));
        return out;
    });

    sr.run("monica", "T");

    int roundLines = 0;
    for (const auto& e : events)
        if (e.find("Round ") != std::string::npos && e.find("dispatch") != std::string::npos)
            ++roundLines;
    CHECK(roundLines >= 1); // one per round actually run, never doubled
    CHECK_FALSE(anyEventContains(events, "no narration from the model"));
}
