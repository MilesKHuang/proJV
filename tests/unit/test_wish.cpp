// proJV -- /wish mode (src/core/wish.h, src/tools/wish_tool.cpp).
//
// The properties that matter, in order of "would silently ruin a run":
//   1. the two tools are invisible outside /wish (the model must not record
//      decisions nobody asked for);
//   2. a wish survives a pass and stops for the right reason (todo done, idle,
//      blocked, pass ceiling);
//   3. /clear really clears it.
#include "doctest/doctest.h"
#include "core/agent.h"
#include "core/wish.h"
#include "client/deepseek.h"
#include "tools/registry.h"
#include "tools/todo_tool.h"

#include <algorithm>
#include <string>

namespace {

bool hasTool(const std::vector<ToolDefinition>& defs, const std::string& name) {
    return std::any_of(defs.begin(), defs.end(),
                       [&](const ToolDefinition& d) { return d.name == name; });
}

void driveTodo(ToolRegistry& tools, const std::string& args) {
    tools.execute("update_todo", args);
}

} // namespace

TEST_CASE("wish: the two tools exist only while a wish is active") {
    DeepSeekClient client;
    ToolRegistry tools;
    Agent agent(client, tools);

    // Both are registered -- filtering, not registration, hides them.
    CHECK(hasTool(tools.getToolDefinitions(), "wish_decide"));
    CHECK(hasTool(tools.getToolDefinitions(), "wish_block"));

    CHECK_FALSE(hasTool(agent.getFilteredToolDefinitions(), "wish_decide"));
    CHECK_FALSE(hasTool(agent.getFilteredToolDefinitions(), "wish_block"));

    agent.startWish("make the parser complete");
    const auto active = agent.getFilteredToolDefinitions();
    CHECK(hasTool(active, "wish_decide"));
    CHECK(hasTool(active, "wish_block"));

    agent.cancelWish();
    CHECK_FALSE(hasTool(agent.getFilteredToolDefinitions(), "wish_decide"));
}

TEST_CASE("wish: startWish records the objective and resets the counters") {
    DeepSeekClient client;
    ToolRegistry tools;
    Agent agent(client, tools);

    agent.startWish("ship a working CLI");
    const Wish w = agent.wishSnapshot();
    CHECK(w.active);
    CHECK(w.objective == "ship a working CLI");
    CHECK(w.passes == 0);
    CHECK(w.idleRounds == 0);
    CHECK(w.decisions == 0);
    CHECK_FALSE(w.blocked);
}

TEST_CASE("wish: wish_decide appends a decision and counts it") {
    DeepSeekClient client;
    ToolRegistry tools;
    Agent agent(client, tools);
    agent.startWish("build the importer");

    const std::string r = tools.execute(
        "wish_decide", R"({"decision":"use SQLite, not Postgres","reason":"zero-install"})");
    CHECK(r.find("[wish] decision #1 recorded") != std::string::npos);
    CHECK(agent.wishSnapshot().decisions == 1);

    tools.execute("wish_decide", R"({"decision":"staging table first"})");
    const auto decisions = agent.wishDecisions();
    REQUIRE(decisions.size() == 2);
    CHECK(decisions[0] == "use SQLite, not Postgres -- zero-install");
    CHECK(decisions[1] == "staging table first");
}

TEST_CASE("wish: both tools refuse to run when no wish is active") {
    DeepSeekClient client;
    ToolRegistry tools;
    Agent agent(client, tools);

    const std::string a = tools.execute("wish_decide", R"({"decision":"x"})");
    const std::string b = tools.execute("wish_block", R"({"reason":"y"})");
    CHECK(a.find("Error: /wish is not active") != std::string::npos);
    CHECK(b.find("Error: /wish is not active") != std::string::npos);
    CHECK(agent.wishDecisions().empty());
}

TEST_CASE("wish: wish_block records the reason and becomes a stop reason") {
    DeepSeekClient client;
    ToolRegistry tools;
    Agent agent(client, tools);
    agent.startWish("deploy it");

    const std::string r = tools.execute("wish_block", R"({"reason":"no API key for the host"})");
    CHECK(r.find("[wish] blocked:") != std::string::npos);
    const Wish w = agent.wishSnapshot();
    CHECK(w.blocked);
    CHECK(w.blockReason == "no API key for the host");
    CHECK(agent.wishStopReason(w).find("blocked") != std::string::npos);
}

TEST_CASE("wish: an idle pass counts, a pass with any tool call does not") {
    DeepSeekClient client;
    ToolRegistry tools;
    Agent agent(client, tools);
    agent.startWish("refactor the loader");

    // Pass 1: no tool call at all -> idle.
    CHECK(agent.endWishPass().idleRounds == 1);
    // Pass 2: the model talked again -> idle twice, which is the stop condition.
    const Wish w2 = agent.endWishPass();
    CHECK(w2.idleRounds == wish_mode::kMaxIdleRounds);
    CHECK(agent.wishStopReason(w2).find("idle") != std::string::npos);

    // A pass that called a tool resets the idle counter.
    agent.startWish("refactor the loader");
    agent.endWishPass();
    tools.execute("wish_decide", R"({"decision":"split the header"})");
    CHECK(agent.endWishPass().idleRounds == 0);
}

TEST_CASE("wish: the pass ceiling stops the loop") {
    DeepSeekClient client;
    ToolRegistry tools;
    Agent agent(client, tools);
    agent.startWish("a very long wish");
    driveTodo(tools, R"({"action":"init","tasks":"1. never finish this"})");

    Wish w = agent.wishSnapshot();
    for (int i = 0; i < wish_mode::kMaxPasses; ++i) {
        tools.execute("wish_decide", R"({"decision":"keep going"})"); // never idle
        w = agent.endWishPass();
    }
    CHECK(w.passes == wish_mode::kMaxPasses);
    CHECK(agent.wishStopReason(w).find("ceiling") != std::string::npos);
}

TEST_CASE("wish: todo done is the definition of done") {
    DeepSeekClient client;
    ToolRegistry tools;
    Agent agent(client, tools);
    agent.startWish("add the export button");

    driveTodo(tools, R"({"action":"init","tasks":"1. add button"})");
    CHECK(agent.hasUnfinishedTodo());
    // One pass runs with that checklist open; the loop counts it at the end.
    const Wish afterInit = agent.endWishPass();
    CHECK(afterInit.sawTodo);
    CHECK(agent.wishStopReason(afterInit).empty()); // keep going

    driveTodo(tools, R"({"action":"done"})");
    CHECK_FALSE(agent.hasUnfinishedTodo());
    CHECK(agent.endWishPass().sawTodo); // the checklist was seen earlier
    CHECK(agent.wishStopReason(agent.wishSnapshot()) == "todo:done");
}

TEST_CASE("wish: a wish with no TODO yet is not immediately 'done'") {
    // /wish starts a turn whose first job is update_todo(init). Before that the
    // checklist is empty, so treating "empty TODO" as done would end every wish
    // after one pass. sawTodo is the memory that makes the difference.
    DeepSeekClient client;
    ToolRegistry tools;
    Agent agent(client, tools);
    agent.startWish("anything");

    const Wish first = agent.endWishPass(); // no tool call in pass 1
    CHECK(first.passes == 1);
    CHECK_FALSE(first.sawTodo);
    CHECK(agent.wishStopReason(first).find("todo:done") == std::string::npos);
}

TEST_CASE("wish: clearSession drops the wish state and the decisions") {
    DeepSeekClient client;
    ToolRegistry tools;
    Agent agent(client, tools);
    agent.startWish("something");
    tools.execute("wish_decide", R"({"decision":"pick A"})");
    driveTodo(tools, R"({"action":"init","tasks":"1. step one"})");

    agent.clearSession();
    CHECK_FALSE(agent.wishSnapshot().active);
    CHECK(agent.wishDecisions().empty());
    CHECK_FALSE(agent.hasUnfinishedTodo());
}
