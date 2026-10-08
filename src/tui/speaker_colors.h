// proJV TUI -- unified speaker accent colors.
//
// A skill role's color is NOT hard-coded per name. Every role name maps to a
// stable slot in a small palette drawn from the ACTIVE THEME, so accents stay
// harmonious with the theme background and follow theme switches for free.
// This is the single source of truth for role coloring: chat_view (bubbles)
// and main_tui (agent sidebar) both call forRole().
#pragma once

#include <ftxui/dom/elements.hpp>

#include <cstddef>
#include <string>

namespace speaker_colors {

// Number of palette slots (theme color fields) available to role names.
std::size_t paletteCount();

// Deterministic slot (0..paletteCount()-1) for a role name: same name always
// maps to the same slot (stable across runs), different names spread across
// slots. Exposed so tests can lock determinism without depending on Color.
std::size_t paletteSlot(const std::string& roleName);

// Accent color for a role name, read from the active theme. Empty name falls
// back to the theme's default text color.
ftxui::Color forRole(const std::string& roleName);

} // namespace speaker_colors
