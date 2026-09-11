// proJV TUI -- chat view: render chat bubbles to FTXUI elements.
#pragma once

#include "tui/bubble_model.h"

#include <ftxui/dom/elements.hpp>

#include <vector>

namespace chat_view {

// Render the bubble list into a single FTXUI element.
// Applies the same sliding window (last N visible) and tool_result merging
// rules as the legacy renderChatArea. Pure function (Element is immutable),
// so it can be golden-tested against an in-memory Screen.
ftxui::Element renderBubbles(const std::vector<bubble_model::Bubble>& bubbles);

// Render the live streaming bubble (reasoning card + content) while the agent
// is still generating. Replaced by the persisted bubble once the turn ends.
ftxui::Element renderStreamingBubble(const AgentStatus& status);

} // namespace chat_view
