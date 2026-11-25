// proJV -- speaker palette slots (design 08.5, D5).
//
// A skill role's accent colour is not hard-coded per name: every role name maps
// to a stable slot in a small palette of THEME fields, so accents stay in
// harmony with the active theme and follow a theme switch for free. This file is
// the pure half -- the slot arithmetic plus the field table. It has no FTXUI and
// no ImGui type, so the TUI (speaker_colors) and the studio (studio_theme) both
// call it instead of keeping two tables that can drift.
#pragma once

#include <cstddef>
#include <string>

#include "tui/theme_colors.h"

namespace speaker_slots {

// Number of palette slots (theme color fields) available to role names.
std::size_t paletteCount();

// Deterministic slot (0..paletteCount()-1) for a role name: same name always
// maps to the same slot (stable across runs), different names spread across
// slots. An empty name maps to 0 (the caller shows the default text colour).
std::size_t paletteSlot(const std::string& roleName);

// The theme field for a slot, clamped into range. Never null.
const std::string ThemeColors::*paletteField(std::size_t slot);

} // namespace speaker_slots
