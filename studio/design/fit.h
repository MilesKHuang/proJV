// proJV studio -- text measuring and fitting, with the measurer injected.
//
// Two GUI problems live here:
//
//   1. A string that does not fit. A terminal wraps and the layout survives; a
//      window has a box, and a label wider than its box either overlaps its
//      neighbour or silently pushes the layout. Everything the shell draws as a
//      single line (a title, a button, a status segment) goes through ellipsize()
//      or bestTier() first, so it always fits the box it was given.
//   2. A tier chosen by hand. "This is a title, use 20 px" is only true while the
//      window is wide enough; bestTier() picks the largest tier that still fits,
//      which is what makes a header behave at 800 px and at 4K.
//
// No ImGui here: the measurer is a callable, so the unit tests use a fake of
// known metrics (8 px per ASCII byte, 16 px per CJK code point) and pin the
// results exactly.
#pragma once

#include <cstddef>
#include <functional>
#include <string>

namespace studio {
namespace fit {

// Pixel width of one UTF-8 string, in the face and size the caller wants.
using Measure = std::function<float(const std::string&)>;

// Width of one UTF-8 string at ONE pixel of font size. Width at size s is then
// unitWidth(text) * s, which is exact for a scalable face and is how bestTier
// compares tiers without re-measuring at every size.
using UnitMeasure = std::function<float(const std::string&)>;

// Byte length of the code point starting at `i` (1..4; a stray byte counts 1).
std::size_t codePointLen(const std::string& utf8, std::size_t i);

// Decode the code point starting at `i`; returns 0xFFFD for an invalid byte.
unsigned int decode(const std::string& utf8, std::size_t i);

// True for East Asian wide characters: the ones a terminal counts as two cells
// and a proportional font draws about twice as wide.
bool isWide(unsigned int cp);

// Display columns of a string: wide = 2, everything else = 1.
int displayColumns(const std::string& utf8);

// `text` when it fits in `width`, otherwise the longest prefix that fits with
// "..." appended. Never splits a code point; returns "..." if even one character
// does not fit, and "" only for empty input or a non-positive width.
std::string ellipsize(const std::string& text, float width, const Measure& measure);

// Cut `text` after the last code point whose measured width is <= `width`.
// Empty input, or width <= 0, gives "".
std::string prefixThatFits(const std::string& text, float width, const Measure& measure);

// Index of the largest tier that fits on ONE line, `sizes` ordered largest
// first. Returns count-1 when even the smallest overflows, 0 for count <= 0.
// `unitWidth` is the string's width at 1 px of font size (see UnitMeasure).
int bestTier(const std::string& text, float width, const float* sizes, int count,
             const UnitMeasure& unitWidth);

} // namespace fit
} // namespace studio
