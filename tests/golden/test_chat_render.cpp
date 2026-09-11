// chat_view golden tests: render bubble list to an in-memory Screen and assert
// the key rendering behaviors match the legacy renderChatArea.
#include "doctest.h"

#include "tui/bubble_model.h"
#include "tui/chat_view.h"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include <string>
#include <vector>

using bubble_model::Bubble;

namespace {

std::string renderToString(const std::vector<Bubble>& bubbles) {
    auto doc = chat_view::renderBubbles(bubbles);
    auto screen = ftxui::Screen::Create(
        ftxui::Dimension::Fixed(80), ftxui::Dimension::Fixed(24));
    ftxui::Render(screen, doc);
    return screen.ToString();
}

Bubble mk(const std::string& role, const std::string& content) {
    Bubble b;
    b.role = role;
    b.content = content;
    return b;
}

} // namespace

TEST_CASE("chat_view: normal bubbles render content") {
    std::vector<Bubble> b;
    b.push_back(mk("user", "hello world"));
    b.push_back(mk("assistant", "hi there"));
    std::string out = renderToString(b);
    CHECK(out.find("hello world") != std::string::npos);
    CHECK(out.find("hi there") != std::string::npos);
}

TEST_CASE("chat_view: reasoning card shows label and body when expanded") {
    std::vector<Bubble> b;
    Bubble think;
    think.role = "assistant";
    think.hasReasoning = true;
    think.reasoningText = "some thinking";
    think.reasoningExpanded = true;
    b.push_back(think);

    std::string out = renderToString(b);
    // "some thinking" is 13 chars.
    CHECK(out.find("Reasoning (13 chars)") != std::string::npos);
    CHECK(out.find("some thinking") != std::string::npos);
}

TEST_CASE("chat_view: reasoning card collapsed hides body") {
    std::vector<Bubble> b;
    Bubble think;
    think.role = "assistant";
    think.hasReasoning = true;
    think.reasoningText = "secret thought";
    think.reasoningExpanded = false;
    b.push_back(think);

    std::string out = renderToString(b);
    CHECK(out.find("[+] Reasoning") != std::string::npos);
    CHECK(out.find("secret thought") == std::string::npos);
}

TEST_CASE("chat_view: tool_call merges following tool_result") {
    std::vector<Bubble> b;
    b.push_back(mk("tool_call", "exec_shell: ls"));
    b.push_back(mk("tool_result", "file1\nfile2"));
    b.push_back(mk("tool_result", "file3"));

    std::string out = renderToString(b);
    CHECK(out.find("exec_shell: ls") != std::string::npos);
    CHECK(out.find("file1") != std::string::npos);
    CHECK(out.find("file2") != std::string::npos);
    CHECK(out.find("file3") != std::string::npos);
    CHECK(out.find("--------------------") != std::string::npos);
}

TEST_CASE("chat_view: sliding window hint for >100 bubbles") {
    std::vector<Bubble> b;
    for (int i = 0; i < 120; ++i) {
        b.push_back(mk("user", "msg" + std::to_string(i)));
    }
    std::string out = renderToString(b);
    CHECK(out.find("[ Showing last 100 of 120 messages") != std::string::npos);
}

TEST_CASE("chat_view: streaming bubble shows reasoning and content") {
    AgentStatus st;
    st.reasoningText = "thinking...";
    st.streamingText = "partial answer";

    auto doc = chat_view::renderStreamingBubble(st);
    auto screen = ftxui::Screen::Create(
        ftxui::Dimension::Fixed(60), ftxui::Dimension::Fixed(12));
    ftxui::Render(screen, doc);
    std::string out = screen.ToString();

    CHECK(out.find("thinking...") != std::string::npos);
    CHECK(out.find("partial answer") != std::string::npos);
}
