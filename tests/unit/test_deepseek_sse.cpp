// SSE parsing tests -- lock the streaming delta callback behavior.
#include "doctest.h"

#include "client/deepseek.h"

#include <string>

namespace {

struct Sink {
    std::string text;
    std::string thinking;
    std::vector<ToolCall> toolCalls;
    bool finished = false;
    std::string finishReason;

    StreamCallbacks callbacks() {
        StreamCallbacks cb;
        cb.onText = [this](const std::string& t) { text += t; };
        cb.onThinking = [this](const std::string& t) { thinking += t; };
        cb.onToolCall = [this](const ToolCall& tc) { toolCalls.push_back(tc); };
        cb.onFinish = [this]() { finished = true; };
        cb.onFinishReason = [this](const std::string& r) { finishReason = r; };
        return cb;
    }
};

} // namespace

TEST_CASE("SSE: content delta calls onText") {
    DeepSeekClient client;
    Sink sink;
    auto cb = sink.callbacks();

    std::string event = "data: {\"choices\":[{\"delta\":{\"content\":\"hello\"}}]}\n\n";
    client.parseSSEChunk(event, 0, cb);
    CHECK(sink.text == "hello");
}

TEST_CASE("SSE: reasoning_content calls onThinking") {
    DeepSeekClient client;
    Sink sink;
    auto cb = sink.callbacks();

    std::string event = "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"plan\"}}]}\n\n";
    client.parseSSEChunk(event, 0, cb);
    CHECK(sink.thinking == "plan");
}

TEST_CASE("SSE: tool_calls parsed") {
    DeepSeekClient client;
    Sink sink;
    auto cb = sink.callbacks();

    std::string event =
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"c1\","
        "\"function\":{\"name\":\"read_file\",\"arguments\":\"{\\\"path\\\":\\\"a.txt\\\"}\"}}]}}]}\n\n";
    client.parseSSEChunk(event, 0, cb);

    REQUIRE(sink.toolCalls.size() == 1);
    CHECK(sink.toolCalls[0].name == "read_file");
    CHECK(sink.toolCalls[0].arguments.find("a.txt") != std::string::npos);
}

TEST_CASE("SSE: finish_reason calls onFinish") {
    DeepSeekClient client;
    Sink sink;
    auto cb = sink.callbacks();

    std::string event = "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n";
    client.parseSSEChunk(event, 0, cb);
    CHECK(sink.finished);
    CHECK(sink.finishReason == "stop");
}

TEST_CASE("SSE: [DONE] returns SIZE_MAX") {
    DeepSeekClient client;
    Sink sink;
    auto cb = sink.callbacks();

    std::string event = "data: [DONE]\n\n";
    CHECK(client.parseSSEChunk(event, 0, cb) == SIZE_MAX);
}
