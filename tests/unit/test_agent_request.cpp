// R2 regression tests: new session must not carry over overview/TODO/user-request context.
//
// Root cause: Agent::clearSession() previously only cleared the message session,
// leaving Agent::todoData (overviewSummary / pendingTodo / pendingBriefs /
// activeUserPrompt) alive. buildChatRequest() then kept injecting
// [OVERVIEW]/[TODO]/[USER REQUEST ON-GOING] system messages into every new session.
//
// These tests drive the update_todo tool through ToolRegistry (which Agent
// registers in its constructor) to populate todoData, then assert clearSession()
// resets all four fields and that the three system-message builders go empty.

#include "doctest.h"
#include "core/agent.h"
#include "client/deepseek.h"
#include "tools/registry.h"
#include "tools/todo_tool.h"

namespace {

void driveTodo(ToolRegistry& tools, const std::string& args) {
    tools.execute("update_todo", args);
}

} // namespace

TEST_CASE("R2: clearSession clears overview/TODO/briefs/user-request context") {
    DeepSeekClient client;
    ToolRegistry tools;
    Agent agent(client, tools); // constructor calls registerTodoTool

    // 1) Produce an archived overview: init -> update -> done
    agent.startTurn("fix the build");
    driveTodo(tools, R"({"action":"init","tasks":"1. Fix A\n2. Fix B"})");
    driveTodo(tools,
              R"({"action":"update","task":"Fix A","status":"done","brief":"A.h: fixed ctor"})");
    driveTodo(tools, R"({"action":"done"})");

    // 2) Produce an active TODO + brief + user-request: init + update, no done
    agent.startTurn("add feature C");
    driveTodo(tools, R"({"action":"init","tasks":"3. Add feature C"})");
    driveTodo(
        tools,
        R"({"action":"update","task":"Add feature C","status":"in_progress","brief":"C.h: added stub"})");

    // Baseline: all four fields are non-empty before clearing (otherwise the
    // test would pass vacuously).
    {
        auto todo = agent.copyTodoData();
        CHECK_FALSE(todo.overviewSummary.empty());
        CHECK_FALSE(todo.pendingTodo.empty());
        CHECK_FALSE(todo.pendingBriefs.empty());
        CHECK_FALSE(todo.activeUserPrompt.empty());
    }

    // The fix under test: clearSession() must reset todoData.
    agent.clearSession();
    auto cleared = agent.copyTodoData();
    CHECK(cleared.overviewSummary.empty());
    CHECK(cleared.pendingTodo.empty());
    CHECK(cleared.pendingBriefs.empty());
    CHECK(cleared.activeUserPrompt.empty());
}

TEST_CASE("R2: cleared TodoData produces no injected system messages") {
    DeepSeekClient client;
    ToolRegistry tools;
    Agent agent(client, tools);

    agent.startTurn("do a task");
    driveTodo(tools, R"({"action":"init","tasks":"1. Task"})");
    driveTodo(tools, R"({"action":"done"})");
    agent.clearSession();

    auto td = agent.copyTodoData();

    // With the injection source cleared, all three builders must return empty,
    // which guarantees buildChatRequest() inserts no [OVERVIEW]/[TODO]/[USER REQUEST].
    CHECK(buildOverviewSystemMessage(td).empty());
    CHECK(buildTodoSystemMessage(td).empty());
    CHECK(buildUserRequestSystemMessage(td).empty());
}

TEST_CASE("models: display-only fields never reach the request body") {
    // duration_ms, skill_run_id and diff exist to be looked at. to_json builds its
    // keys explicitly, so they cannot leak into a request and quietly cost tokens.
    Message t = Message::Tool("call_1", "exec_shell", "done");
    t.durationMs = 12400;
    t.skillRunId = "skill-1761226800000";
    t.diff = "--- a/x\n+++ b/x\n@@ -1 +1 @@\n-a\n+b\n";
    const nlohmann::json j = t;
    CHECK(j.contains("role"));
    CHECK_FALSE(j.contains("duration_ms"));
    CHECK_FALSE(j.contains("skill_run_id"));
    CHECK_FALSE(j.contains("diff"));
}
