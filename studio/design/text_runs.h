// proJV studio -- text selection arithmetic on RENDERED lines (see text_runs.cpp).
//
// Why this file exists. A selection is expressed in CHARACTER indices, but the
// mouse speaks pixels. The old code converted between them with a guessed ruler
// ("columns x the width of 'M'"), which is exact in a terminal and wrong in a
// font: a CJK glyph is one font-wide box, not two Latin cells, so a drag over
// Chinese drifted further from the pointer with every glyph -- "the highlight
// does not match what I copied".
//
// The fix is not an algorithm, it is the right input: each visual line already
// knows where every character starts (charX, measured once when the line was
// laid out, with the very font that draws it). Selection then reads those
// offsets instead of inventing its own, so the highlight, the hit-test and the
// clipboard are three readers of ONE ruler.
//
// ImGui-free on purpose: the arithmetic that decides which characters a drag
// covers can be pinned by unit tests without a font, a draw list or a frame.
#pragma once

#include <string>
#include <vector>

#include "design/text_select.h"

namespace studio {
namespace text_runs {

// One rendered visual line, in visual order. `charX` has one entry per code
// point plus a trailing entry for the line's total width, all in pixels from the
// line's left edge (the same origins the highlight rectangle uses).
struct Line {
    int block = 0;
    int line = 0;
    std::string text;          // the line's plain text, exactly as drawn
    std::vector<float> charX;  // size == charCount(text) + 1
    bool soft = false;         // continues the previous visual line (a wrap, no newline)
};

int charCount(const std::string& utf8);

// Pixel x (relative to the line's left edge) -> character index. Returns a value
// in [0, charCount]. Linear before the first and after the last character is not
// needed: a click past either end belongs to that end.
int columnAt(const std::vector<float>& charX, float dx);

// Characters [c0, c1) of one line (code-point indices, clamped).
std::string sliceChars(const std::string& utf8, int c0, int c1);

// The text a normalised selection covers. `lines` must be in visual order; lines
// that are not in `lines` (a folded row, an old streamed block) cannot leak into
// the clipboard, which is the second half of the "copied text does not match the
// highlight" bug. Soft-wrapped lines are joined without a newline so a wrapped
// sentence is copied as one sentence; every other line break is kept.
std::string extract(const std::vector<Line>& lines, const Selection& sel);

} // namespace text_runs
} // namespace studio
