// proJV TUI -- unified speaker accent colors (see speaker_colors.h).
#include "speaker_colors.h"

#include "theme_colors.h"
#include "theme_manager.h"
#include "theme_map.h"

namespace speaker_colors {
namespace {

// Curated speaker palette: theme fields with distinct hues, all authored for
// the active theme, so a role accent never clashes with the background. These
// are deliberately semantic fields (accent / link / done / warn / magenta /
// cyan), not the gray/background-ish fields (statusModelName, workspace, ...).
const std::string ThemeColors::*kPalette[] = {
    &ThemeColors::phaseStreaming,     // gold / accent
    &ThemeColors::mdLink,             // blue
    &ThemeColors::todoDone,           // green
    &ThemeColors::statusCtxHigh,      // orange
    &ThemeColors::phaseAwaitApproval, // magenta
    &ThemeColors::mdItalic,           // cyan
};
constexpr std::size_t kPaletteSize = sizeof(kPalette) / sizeof(kPalette[0]);

// FNV-1a: cheap, stable, well-spread for short ASCII/UTF-8 names.
std::size_t fnv1a(const std::string& s) {
    std::size_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= static_cast<std::size_t>(c);
        h *= 1099511628211ull;
    }
    return h;
}

} // namespace

std::size_t paletteCount() { return kPaletteSize; }

std::size_t paletteSlot(const std::string& roleName) {
    if (roleName.empty()) return 0;
    return fnv1a(roleName) % kPaletteSize;
}

ftxui::Color forRole(const std::string& roleName) {
    const ThemeColors& T = ThemeManager::instance().current();
    if (roleName.empty()) return theme_map::hexToColor(T.text);
    return theme_map::hexToColor(T.*kPalette[paletteSlot(roleName)]);
}

} // namespace speaker_colors
