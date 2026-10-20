// Tests for the run_skill turn split (the "end the turn, run the skill, resume"
// design).
//
// Contract being locked down:
//   1. a tool can ask the Agent to end the turn (ToolRegistry turn-end flag);
//   2. the request is consumed exactly once, and clearing it is idempotent;
//   3. the session left behind by an early-ended turn is still a legal
//      assistant(tool_calls) -> tool pair (no orphaned tool_calls).
#include "doctest.h"

#include "tools/registry.h"
#include "core/session.h"
#include "client/models.h"

#include <string>

namespace {

ToolCall fakeCall(const std::string& name) {
    ToolCall tc;
    tc.id = "call_" + name;
    tc.name = name;
    tc.arguments = "{}";
    tc.index = 0;
    tc.valid = true;
    return tc;
}

} // namespace

TEST_CASE("turn end: a tool can request it and the Agent consumes it once") {
    ToolRegistry tools;
    CHECK_FALSE(tools.consumeTurnEndRequest());      // nothing pending

    ToolDefinition def;
    def.name = "run_skill";
    def.description = "stub";
    tools.registerTool(def, [&tools](const std::string&) {
        tools.requestTurnEnd();
        return "[skill] queued";
    });

    const std::string out = tools.execute("run_skill", "{}");
    CHECK(out == "[skill] queued");
    CHECK(tools.consumeTurnEndRequest());            // honoured exactly once
    CHECK_FALSE(tools.consumeTurnEndRequest());      // and cleared
}

TEST_CASE("turn end: cancelling a turn must not leave a pending request") {
    ToolRegistry tools;
    tools.requestTurnEnd();
    // Agent::newTurn() does exactly this before starting the next turn.
    (void)tools.consumeTurnEndRequest();
    CHECK_FALSE(tools.consumeTurnEndRequest());
}

TEST_CASE("turn end: the placeholder tool result keeps the pairing legal") {
    Session s;
    s.addMessage(Message::System("sys"));
    s.addMessage(Message::User("please run monica"));

    Message asst = Message::Assistant("");
    asst.toolCalls.push_back(fakeCall("run_skill"));
    s.addMessage(asst);
    // What the executor returns, written by Agent before the early break.
    s.addMessage(Message::Tool("call_run_skill", "run_skill",
                               "[skill] 'monica' is starting now.", false));

    CHECK_FALSE(s.hasOrphanedTools());
    CHECK(s.messageCount() == 4);
}

TEST_CASE("turn end: the skill narration that follows is legal too") {
    Session s;
    s.addMessage(Message::User("/monica D:/x"));
    s.addMessage(Message::Assistant("**[Sheldon]** proposal"));
    s.addMessage(Message::Assistant("# final document"));

    CHECK_FALSE(s.hasOrphanedTools());
    CHECK(s.messageCount() == 3);
}

TEST_CASE("turn end: dropping the placeholder would orphan the tool_calls") {
    // Locks the reason the executor must NOT return an empty result set.
    Session s;
    Message asst = Message::Assistant("");
    asst.toolCalls.push_back(fakeCall("run_skill"));
    s.addMessage(asst);
    s.addMessage(Message::System("[skill] finished"));   // never do this
    CHECK(s.hasOrphanedTools());
}
