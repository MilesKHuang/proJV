// proJV -- the shared-context digest handed to skill agents.
//
// The bug this replaces: buildSharedContext() pasted the WHOLE conversation into
// every sub-agent's prompt. In a 270k-token session, monica's 11 agents each carried
// all 270k, which is why a run took minutes before the first round even started.
// Now the skill gets one bounded digest: the model-written per-turn blocks
// ([USER REQUEST ON-GOING], [TODO], [OVERVIEW]) plus the newest 12 KB of the
// conversation, and -- when that is still large -- a compactor-style summary of it.
//
// The network path is not unit-tested (it needs the API); what is tested here is the
// contract that matters: the digest is small, it carries the request/TODO, and it
// never modifies the session.
#include "doctest/doctest.h"
#include "client/deepseek.h"
#include "core/agent.h"
#include "tools/registry.h"
#include "tools/todo_tool.h"

#include <string>

TEST_CASE("shared context: empty session yields an empty digest") {
    DeepSeekClient client;
    ToolRegistry tools;
    Agent agent(client, tools);
    CHECK(agent.summarizeConversationForSharing().empty());
}

TEST_CASE("shared context: the digest carries the request and the TODO, not the whole log") {
    DeepSeekClient client;
    ToolRegistry tools;
    Agent agent(client, tools);

    agent.startTurn("make the parser complete");
    tools.execute("update_todo", R"({"action":"init","tasks":"1. lexer\n2. parser"})");

    const std::string digest = agent.summarizeConversationForSharing();
    CHECK(digest.find("make the parser complete") != std::string::npos);
    CHECK(digest.find("lexer") != std::string::npos);
    // A small session stays under the "no API call" threshold, so this is exact.
    CHECK(digest.size() < 6000);
}

TEST_CASE("shared context: a huge single message is capped, not pasted whole") {
    DeepSeekClient client;
    ToolRegistry tools;
    Agent agent(client, tools);
    // 40 KB of "history": the digest must not carry it verbatim.
    agent.addPersistedMessage(Message::User(std::string(40000, 'x')));

    const std::string digest = agent.summarizeConversationForSharing();
    CHECK(digest.size() < 14000); // 12 KB slice + per-message cap, never 40 KB
    CHECK(digest.find("40000") == std::string::npos);
}

TEST_CASE("shared context: summarising does not modify the session") {
    DeepSeekClient client;
    ToolRegistry tools;
    Agent agent(client, tools);
    agent.startTurn("hello");
    agent.addPersistedMessage(Message::User(std::string(20000, 'y')));
    const size_t before = agent.getSession().messageCount();

    (void)agent.summarizeConversationForSharing();
    CHECK(agent.getSession().messageCount() == before);
}
