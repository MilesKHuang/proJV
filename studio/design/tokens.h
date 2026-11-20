// proJV studio -- the design tokens: the one place a size is invented.
//
// Why this file exists. The window used to place things with numbers typed at
// each call site: 3, 4, 5, 6, 8, 12, 14 px all appeared, some of them already
// multiplied by a DPI guess, some not, and the same idea (a gap between two
// controls) had three different values on three screens. That is what "the GUI
// looks inconsistent" means when it is read as code.
//
// From here on a caller names the step it wants -- SpaceMd, RadiusSm, TypeBody --
// and gets pixels from the ladder below, already scaled by (monitor DPI * user
// zoom). Nothing else in studio/ multiplies a size by a scale factor by hand.
//
// This header is ImGui-free on purpose: the ladder is arithmetic, so the unit
// tests link it directly and pin the numbers the whole window is built from.
#pragma once

namespace studio {
namespace tokens {

// ---- the ladders ----------------------------------------------------------
// Values are at 100% (a 96 dpi monitor, no zoom). The ratio is what makes a
// screen look designed: 4 / 8 / 12 / 16 / 24 / 32 for space (a 4 px rhythm), and
// a type scale of roughly 1.2 between tiers.
enum class Type {
    Display,  // the application name in the header
    Title,    // a dialog title, a page title
    Heading,  // a section header inside a surface
    Body,     // default: chat text, buttons, list rows
    Small,    // secondary: status bar, captions, hints
    Caption,  // the smallest tier: keycaps, footnotes
    Mono,     // body size, but the monospace face (prompt, aligned output)
};

enum class Space {
    None = 0,  // 0 px, for "deliberately none"
    Xs = 1,    // 4
    Sm = 2,    // 8
    Md = 3,    // 12
    Lg = 4,    // 16
    Xl = 5,    // 24
    Xxl = 6,   // 32
};

enum class Radius {
    Xs = 0,   // 4 -- keycaps, small chips
    Sm = 1,   // 8 -- buttons, inputs
    Md = 2,   // 12 -- cards, the composer
    Lg = 3,   // 16 -- dialogs
    Pill = 4, // fully round (badges)
};

constexpr int kTypeCount = 7;
constexpr int kSpaceCount = 7;
constexpr int kRadiusCount = 5;

// The unscaled ladders, exposed so tests and the theme loader can read them.
extern const float kTypePx[kTypeCount];
extern const float kSpacePx[kSpaceCount];
extern const float kRadiusPx[kRadiusCount];

// Line height multiplier per tier: display and headings are set tight, body text
// is set for reading. A single multiplier is why headings looked like paragraphs.
extern const float kLineRatio[kTypeCount];

// ---- resolved metrics -----------------------------------------------------

// Everything the window needs, in pixels, for one (dpi, zoom) pair.
struct Metrics {
    float factor = 1.0f;         // dpi * zoom, already clamped to a sane range
    float type[kTypeCount] = {}; // font size per tier
    float line[kTypeCount] = {}; // line height per tier
    float space[kSpaceCount] = {};
    float radius[kRadiusCount] = {};
    float controlH = 0.0f;    // button / input height
    float rowH = 0.0f;        // one list or hint row
    float iconSize = 0.0f;    // square glyph box
    float hairline = 1.0f;    // separator / border width (never below 1 px)
    float pagePad = 0.0f;     // window edge margin
    float gutter = 0.0f;      // gap between major regions
    float readingMax = 0.0f;  // widest comfortable text column
    float dockW = 0.0f;       // the right dock's width
    float scrollbar = 0.0f;   // scrollbar thickness
};

// Resolve the ladders for one scale. `dpi` is the monitor scale (1.0 = 96 dpi),
// `zoom` is the user's own text size (Ctrl +/-), default 1.0.
// Out-of-range values are clamped, never asserted: a driver reporting 0 dpi must
// still produce a usable window.
Metrics build(float dpi, float zoom = 1.0f);

// Convenience accessors: an out-of-range enum value falls back to the default
// tier instead of reading past the array.
float typePx(const Metrics& m, Type t);
float linePx(const Metrics& m, Type t);
float spacePx(const Metrics& m, Space s);
float radiusPx(const Metrics& m, Radius r);

// The size the font atlas is loaded at, and the size a tier is drawn at, share
// one number: the atlas is baked at kAtlasPx and every tier is a multiple of it,
// so ImGui 1.92+ rasterizes each tier once and stays crisp.
constexpr float kAtlasPx = 18.0f;

} // namespace tokens
} // namespace studio
