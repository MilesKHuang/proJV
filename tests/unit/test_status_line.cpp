// Status line / status bar text tests.
#include "doctest.h"

#include "tui/status_line.h"
#include "tui/status_bar.h"

#include <string>

TEST_CASE("status_line: idle") {
    AgentStatus st;
    CHECK(status_line::render(st) == "Idle");
}

TEST_CASE("status_line: thinking reasoning only") {
    AgentStatus st;
    st.state = AgentState::Thinking;
    st.reasoningText = "12345";
    CHECK(status_line::render(st) == "Reasoning... (5 chars)");
}

TEST_CASE("status_line: thinking generating + reasoned count") {
    AgentStatus st;
    st.state = AgentState::Thinking;
    st.streamingText = "abcdef";
    st.reasoningText = "abc";
    std::string s = status_line::render(st);
    CHECK(s.find("Generating... (6 chars)") != std::string::npos);
    CHECK(s.find("reasoned 3 chars") != std::string::npos);
}

TEST_CASE("status_line: executing tool") {
    AgentStatus st;
    st.state = AgentState::ExecutingTool;
    st.currentToolName = "read_file";
    st.toolProgressCurrent = 1;
    st.toolProgressTotal = 3;
    CHECK(status_line::render(st) == "Running: read_file (1/3)");
}

TEST_CASE("status_line: awaiting approval") {
    AgentStatus st;
    st.state = AgentState::AwaitingApproval;
    CHECK(status_line::render(st) == "Awaiting approval...");
}

TEST_CASE("status_bar: model/token/msgs/workspace") {
    status_bar::Data d;
    d.model = "deepseek-v4-flash";
    d.promptTokens = 1500;
    d.completionTokens = 500;
    d.msgCount = 10;
    d.workspace = "";
    std::string s = status_bar::render(d);
    CHECK(s.find("deepseek-v4-flash") != std::string::npos);
    CHECK(s.find("1.50K+500") != std::string::npos);
    CHECK(s.find("msgs: 10") != std::string::npos);
    CHECK(s.find("[ws: exe dir]") != std::string::npos);
}

TEST_CASE("status_bar: tools and context pressure") {
    status_bar::Data d;
    d.model = "m";
    d.toolCount = 4;
    d.status.state = AgentState::Thinking;
    d.pressure = Session::PressureLevel::High;
    d.estimatedTokens = 60000;
    d.windowTokens = 100000;
    std::string s = status_bar::render(d);
    CHECK(s.find("tools: 4") != std::string::npos);
    CHECK(s.find("ctx: HIGH") != std::string::npos);
    CHECK(s.find("(60%)") != std::string::npos);
}
