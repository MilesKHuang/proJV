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
    Agent agent(client, tools);   // constructor calls registerTodoTool

    // 1) Produce an archived overview: init -> update -> done
    agent.startTurn("fix the build");
    driveTodo(tools, R"({"action":"init","tasks":"1. Fix A\n2. Fix B"})");
    driveTodo(tools, R"({"action":"update","task":"Fix A","status":"done","brief":"A.h: fixed ctor"})");
    driveTodo(tools, R"({"action":"done"})");

    // 2) Produce an active TODO + brief + user-request: init + update, no done
    agent.startTurn("add feature C");
    driveTodo(tools, R"({"action":"init","tasks":"3. Add feature C"})");
    driveTodo(tools, R"({"action":"update","task":"Add feature C","status":"in_progress","brief":"C.h: added stub"})");

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
