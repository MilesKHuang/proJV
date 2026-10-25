// Serialization contract for `reasoning_content`.
//
// Regression: proJV always sends `tools`, and in thinking mode (the server
// default) the API rejects any request whose assistant messages omit
// `reasoning_content`:
//   HTTP 400: The `reasoning_content` in the thinking mode must be passed back
//             to the API.
// Verified against the live API: the field may be an empty string, and it is
// ignored when the request carries no tools. Skill narration, /help output and
// the stored API-error notices all created assistant messages without a chain of
// thought, so the field must be emitted unconditionally for that role.
#include "doctest.h"

#include "client/models.h"
#include "json.hpp"

#include <string>

TEST_CASE("reasoning_content: assistant with a chain of thought sends it") {
    Message m = Message::Assistant("answer");
    m.reasoningContent = "because ...";
    nlohmann::json j;
    to_json(j, m);
    REQUIRE(j.contains("reasoning_content"));
    CHECK(j["reasoning_content"] == "because ...");
}

TEST_CASE("reasoning_content: assistant without one still sends an empty field") {
    Message m = Message::Assistant("**[Sheldon]** proposal"); // skill narration
    nlohmann::json j;
    to_json(j, m);
    REQUIRE(j.contains("reasoning_content"));
    CHECK(j["reasoning_content"] == "");
    CHECK(j["content"] == "**[Sheldon]** proposal");
}

TEST_CASE("reasoning_content: assistant with tool_calls and no reasoning") {
    Message m = Message::Assistant();
    ToolCall tc;
    tc.id = "call_1";
    tc.name = "run_skill";
    tc.arguments = "{}";
    m.toolCalls.push_back(tc);

    nlohmann::json j;
    to_json(j, m);
    CHECK(j["content"].is_null()); // unchanged: null = no text
    REQUIRE(j.contains("reasoning_content"));
    CHECK(j["reasoning_content"] == "");
    REQUIRE(j["tool_calls"].is_array());
    CHECK(j["tool_calls"][0]["function"]["name"] == "run_skill");
}

TEST_CASE("reasoning_content: never emitted for non-assistant roles") {
    nlohmann::json j;

    to_json(j, Message::User("hi"));
    CHECK_FALSE(j.contains("reasoning_content"));

    j.clear();
    to_json(j, Message::System("rules"));
    CHECK_FALSE(j.contains("reasoning_content"));

    j.clear();
    to_json(j, Message::Tool("call_1", "read_file", "ok"));
    CHECK_FALSE(j.contains("reasoning_content"));
}

TEST_CASE("reasoning_content: an API error notice is a legal assistant message") {
    // Agent persists the failure text as an assistant message; it must not turn
    // the next request into another 400.
    Message m = Message::Assistant("Error: API request failed after retry.\nError: HTTP 400: ...");
    nlohmann::json j;
    to_json(j, m);
    REQUIRE(j.contains("reasoning_content"));
    CHECK(j["reasoning_content"] == "");
    CHECK(j["role"] == "assistant");
}
