// Bubble derivation unit tests -- lock the 1:1 rules from legacy render_chat.cpp.
#include "doctest.h"

#include "tui/bubble_model.h"
#include "models.h"

using namespace bubble_model;

namespace {

ToolCall makeCall(const std::string& name, const std::string& args, int idx) {
    ToolCall tc;
    tc.id = "call_" + std::to_string(idx);
    tc.name = name;
    tc.arguments = args;
    tc.index = idx;
    return tc;
}

} // namespace

TEST_CASE("deriveBubbles: system messages are filtered") {
    // Ordinary system messages are visual noise -> dropped.
    CHECK(deriveBubbles(Message::System("some internal note")).empty());

    // [Workspace] is shown.
    auto ws = deriveBubbles(Message::System("[Workspace] /workspace set to D:/repo"));
    REQUIRE(ws.size() == 1);
    CHECK(ws[0].role == "system");
    CHECK(ws[0].content == "[Workspace] /workspace set to D:/repo");

    // [Context compacted: marker is shown.
    auto compacted = deriveBubbles(Message::System("[Context compacted: 3 messages]"));
    REQUIRE(compacted.size() == 1);
    CHECK(compacted[0].role == "system");
    CHECK(compacted[0].content.find("[Context compacted:") != std::string::npos);
}

TEST_CASE("deriveBubbles: user message is a single bubble") {
    auto b = deriveBubbles(Message::User("hello"));
    REQUIRE(b.size() == 1);
    CHECK(b[0].role == "user");
    CHECK(b[0].content == "hello");
    CHECK_FALSE(b[0].hasReasoning);
}

TEST_CASE("deriveBubbles: text-only assistant with reasoning prepends think bubble") {
    Message m = Message::Assistant("the answer");
    m.reasoningContent = "chain of thought";

    auto b = deriveBubbles(m);
    REQUIRE(b.size() == 2);
    CHECK(b[0].role == "assistant");
    CHECK(b[0].hasReasoning);
    CHECK(b[0].reasoningText == "chain of thought");
    CHECK(b[0].content.empty());
    CHECK(b[1].role == "assistant");
    CHECK(b[1].content == "the answer");
    CHECK_FALSE(b[1].hasReasoning);
}

TEST_CASE("deriveBubbles: assistant tool_call ordering (think -> text -> tool_call)") {
    Message m = Message::Assistant("I'll run a command");
    m.reasoningContent = "planning";
    m.toolCalls.push_back(makeCall("exec_shell", R"({"command":"ls -la"})", 0));

    auto b = deriveBubbles(m);
    REQUIRE(b.size() == 3);
    CHECK(b[0].hasReasoning);
    CHECK(b[0].reasoningText == "planning");
    CHECK(b[1].role == "assistant");
    CHECK(b[1].content == "I'll run a command");
    CHECK(b[2].role == "tool_call");
}

TEST_CASE("formatToolMsg: tool_call key-argument extraction") {
    Message m = Message::Assistant("");
    m.toolCalls.push_back(makeCall("read_file", R"({"path":"src/main.cpp"})", 0));
    auto [role, text] = formatToolMsg(m);
    CHECK(role == "tool_call");
    CHECK(text == "read_file: src/main.cpp");

    Message m2 = Message::Assistant("");
    m2.toolCalls.push_back(makeCall("exec_shell", R"({"command":"cmake --build ."})", 0));
    auto [role2, text2] = formatToolMsg(m2);
    CHECK(role2 == "tool_call");
    CHECK(text2 == "exec_shell: cmake --build .");

    Message m3 = Message::Assistant("");
    m3.toolCalls.push_back(makeCall("grep_files", R"({"pattern":"TODO"})", 0));
    auto [role3, text3] = formatToolMsg(m3);
    CHECK(role3 == "tool_call");
    CHECK(text3 == "grep_files: TODO");
}

TEST_CASE("formatToolMsg: unparseable args fall back to truncated raw") {
    Message m = Message::Assistant("");
    std::string raw = std::string(80, 'x');  // > 60 chars -> truncate to 57 + "..."
    m.toolCalls.push_back(makeCall("unknown_tool", raw, 0));
    auto [role, text] = formatToolMsg(m);
    CHECK(role == "tool_call");
    CHECK(text.find("unknown_tool: ") == 0);
    CHECK(text.find("...") != std::string::npos);
}

TEST_CASE("formatToolMsg: tool_result summary (read_file)") {
    Message t = Message::Tool("c1", "read_file", "line1\nline2\nline3");
    auto [role, text] = formatToolMsg(t);
    CHECK(role == "tool_result");
    // "line1\nline2\nline3" = 17 bytes, 2 newlines.
    CHECK(text == "read_file (2 lines, 17 bytes)  -> line1");
}

TEST_CASE("formatToolMsg: tool_result summary (exec_shell)") {
    Message t = Message::Tool("c1", "exec_shell", "output line\nmore");
    auto [role, text] = formatToolMsg(t);
    CHECK(role == "tool_result");
    // "output line\nmore" = 16 bytes.
    CHECK(text == "exec_shell (16 bytes)  -> output line");
}
