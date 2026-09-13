// proJV TUI -- status line text + theme color (1:1 with the input-area status line).
#pragma once

#include "models.h"
#include "theme_colors.h"

#include <string>

namespace status_line {

// One colored line: `color` points at the ThemeColors field that should tint
// the text, so it follows the active theme (same pattern as status_bar).
struct Line {
    std::string text;
    std::string ThemeColors::* color;
};

// Render the one-line agent status text + theme color for the given snapshot.
Line render(const AgentStatus& status);

} // namespace status_line
