// proJV TUI -- bubble model: Message -> display bubbles (pure logic, no UI deps)
//
// This is a 1:1 extraction of the legacy ImGui logic in
// src/ui/render_chat.cpp (App::deriveBubblesFromMessage + formatToolMsg).
// Keeping the derivation rules as pure functions lets unit tests lock the
// behavior down while the rendering shell (ImGui -> FTXUI) changes.
#pragma once

#include "models.h"

#include <string>
#include <utility>
#include <vector>

namespace bubble_model {

// One rendered chat bubble (equivalent to the legacy ChatBubble).
struct Bubble {
    std::string role;          // "user" | "assistant" | "system" | "tool_call" | "tool_result"
    std::string content;
    std::string reasoningText; // reasoning_content from deepseek-reasoner
    bool hasReasoning = false;
    bool reasoningExpanded = true; // collapsible chain-of-thought (default expanded)
};

// Format a tool message into (bubbleRole, displayText). Pure function.
std::pair<std::string, std::string> formatToolMsg(const Message& msg);

// Derive one or more bubbles from a single Message. Pure function, 1:1 with
// the legacy App::deriveBubblesFromMessage.
std::vector<Bubble> deriveBubbles(const Message& msg);

} // namespace bubble_model
