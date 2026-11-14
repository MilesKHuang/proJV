// proJV TUI -- bubble model: Message -> display bubbles (pure logic, no UI deps)
//
// Kept as pure functions so the derivation rules are unit-testable and the
// rendering shell (FTXUI) can change without touching them: deriveBubbles() is the
// single place that decides which Message becomes which bubble.
#pragma once

#include "models.h"

#include <set>
#include <string>
#include <utility>
#include <vector>

namespace bubble_model {

// One rendered chat bubble (equivalent to the legacy ChatBubble).
struct Bubble {
    // "user" | "assistant" | "system" | "tool_call" | "tool_result" | "diff"
    std::string role;
    std::string content;
    std::string reasoningText; // reasoning_content (thinking mode)
    bool hasReasoning = false;
    std::string speaker; // skill role: "Sheldon" | "Penny" | "Leonard" (else empty)
    bool isVote = false; // true for a "**[Role vote]**" line
    // The framework injected this user message (view_image round), not the user.
    bool synthetic = false;
};

// Format a tool message into (bubbleRole, displayText). Pure function.
std::pair<std::string, std::string> formatToolMsg(const Message& msg);

// Derive one or more bubbles from a single Message. Pure function, 1:1 with
// the legacy App::deriveBubblesFromMessage.
//
// `knownRoles` is the registry of active skill role names. An assistant message
// whose first token is "**[<name>]** ..." or "**[<name> vote]** ..." is tagged
// as a speaker ONLY when <name> is registered, so ordinary bold markdown
// ("**[Note]** ...") is never misread as a role. Empty registry => no speaker
// detection (plain messages pass through untouched).
std::vector<Bubble> deriveBubbles(const Message& msg, const std::set<std::string>& knownRoles = {});

} // namespace bubble_model
