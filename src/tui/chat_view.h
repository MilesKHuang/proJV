// proJV TUI -- chat view: render chat bubbles to FTXUI elements.
#pragma once

#include "tui/bubble_model.h"

#include <ftxui/dom/elements.hpp>

#include <vector>

namespace chat_view {

// Render the bubble list into a single FTXUI element.
// Row-level scrolling is handled by the caller via focusPosition().
// `width` is the chat column's current width in cells (0 = not measured yet).
// Diff bubbles use it to choose a side-by-side layout when there is room and the
// unified block when there is not.
ftxui::Element renderBubbles(const std::vector<bubble_model::Bubble>& bubbles,
                             bool reasoningExpanded, int width = 0);

// Render the live streaming bubble (reasoning card + content) while the agent
// is still generating. Replaced by the persisted bubble once the turn ends.
ftxui::Element renderStreamingBubble(const AgentStatus& status);

} // namespace chat_view
