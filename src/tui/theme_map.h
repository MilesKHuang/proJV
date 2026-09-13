// proJV TUI -- theme color mapping (hex -> FTXUI Color).
#pragma once

#include <ftxui/dom/elements.hpp>

#include <string>

namespace theme_map {

// Parse "#RRGGBB" into an FTXUI Color. Mirrors ThemeColors::toVec4 parsing:
// returns white on malformed input.
ftxui::Color hexToColor(const std::string& hex);

} // namespace theme_map
