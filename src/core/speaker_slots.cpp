// proJV -- speaker palette slots (see speaker_slots.h).
#include "speaker_slots.h"

namespace speaker_slots {
namespace {

// Curated speaker palette: theme fields with distinct hues, all authored for
// the active theme, so a role accent never clashes with the background. These
// are deliberately semantic fields (accent / link / done / warn / magenta /
// cyan), not the gray/background-ish fields (statusModelName, workspace, ...).
// Moved verbatim out of tui/speaker_colors.cpp: the slot order is part of the
// on-screen result, so it must not be re-derived per front-end.
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

std::size_t paletteCount() {
    return kPaletteSize;
}

std::size_t paletteSlot(const std::string& roleName) {
    if (roleName.empty()) return 0;
    return fnv1a(roleName) % kPaletteSize;
}

const std::string ThemeColors::*paletteField(std::size_t slot) {
    return kPalette[slot < kPaletteSize ? slot : 0];
}

} // namespace speaker_slots
