// proJV TUI -- the one spinner glyph set.
//
// FTXUI's charset 8 is a four frame quadrant rotation
// (U+2596 U+2598 U+259D U+2597, the glyphs U+2596/2598/259D/2597), all one column
// wide -- screen/string.cpp's full-width table does not list them, so no custom
// width table is needed. Keeping the charset in one place means a later change
// is one edit instead of six call sites.
#pragma once

#include <ftxui/dom/elements.hpp>

namespace tui {

// Frames per revolution of the charset below. Call sites do
// `frame = (frame + 1) % tui::kSpinnerFrames` so the counter and the glyph table
// cannot drift apart.
inline constexpr int kSpinnerFrames = 4;

inline ftxui::Element spinner(int frame, ftxui::Color c) {
    return ftxui::spinner(8, frame) | ftxui::color(c);
}

} // namespace tui
