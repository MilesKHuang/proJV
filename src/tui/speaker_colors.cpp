// proJV TUI -- unified speaker accent colors (see speaker_colors.h).
//
// Only the FTXUI conversion lives here. The slot arithmetic and the palette
// table moved to core/speaker_slots.cpp (design 08.5, D5) so the studio paints a
// role with the same theme field this file does -- one table, two front-ends.
#include "speaker_colors.h"

#include "core/speaker_slots.h"
#include "theme_colors.h"
#include "theme_manager.h"
#include "theme_map.h"

namespace speaker_colors {

std::size_t paletteCount() {
    return speaker_slots::paletteCount();
}

std::size_t paletteSlot(const std::string& roleName) {
    return speaker_slots::paletteSlot(roleName);
}

ftxui::Color forRole(const std::string& roleName) {
    const ThemeColors& T = ThemeManager::instance().current();
    if (roleName.empty()) return theme_map::hexToColor(T.text);
    return theme_map::hexToColor(T.*speaker_slots::paletteField(paletteSlot(roleName)));
}

} // namespace speaker_colors
