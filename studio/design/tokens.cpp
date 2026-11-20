// proJV studio -- design token ladders (see tokens.h).
#include "tokens.h"

#include <algorithm>
#include <cmath>

namespace studio {
namespace tokens {

// 100% values. Read as: what a desktop application looks like at 96 dpi.
// The body tier is 16 px, not the 14.5 of the previous pass: at 100% on a large
// monitor the old ladder rendered a chat line most people had to lean in to read.
// Every other tier is a step from it, and the ratio stays ~1.2 so the hierarchy
// is visible without any tier looking like a different application.
const float kTypePx[kTypeCount] = {
    28.0f, // Display
    22.0f, // Title
    18.0f, // Heading
    16.0f, // Body
    13.5f, // Small
    12.5f, // Caption
    16.0f, // Mono (same size as Body: it is body text in another face)
};

const float kSpacePx[kSpaceCount] = {
    0.0f,  // None
    4.0f,  // Xs
    8.0f,  // Sm
    12.0f, // Md
    16.0f, // Lg
    24.0f, // Xl
    32.0f, // Xxl
};

const float kRadiusPx[kRadiusCount] = {
    4.0f,   // Xs
    8.0f,   // Sm
    12.0f,  // Md
    16.0f,  // Lg
    999.0f, // Pill
};

const float kLineRatio[kTypeCount] = {
    1.25f, // Display
    1.30f, // Title
    1.35f, // Heading
    1.55f, // Body
    1.50f, // Small
    1.45f, // Caption
    1.55f, // Mono
};

namespace {

int typeIndex(Type t) {
    const int i = static_cast<int>(t);
    return (i < 0 || i >= kTypeCount) ? static_cast<int>(Type::Body) : i;
}

int spaceIndex(Space s) {
    const int i = static_cast<int>(s);
    return (i < 0 || i >= kSpaceCount) ? 0 : i;
}

int radiusIndex(Radius r) {
    const int i = static_cast<int>(r);
    return (i < 0 || i >= kRadiusCount) ? 0 : i;
}

float clampScale(float v) {
    if (!(v > 0.0f)) return 1.0f; // also catches NaN
    return std::min(4.0f, std::max(0.5f, v));
}

// Half-pixel ladder: a 1.33 scale on a 16.5 px heading is 21.945, which ImGui
// would rasterize at a fractional size and hint badly. Rounding to 0.5 keeps the
// scale exact enough to line up and coarse enough to stay crisp.
float snap(float px) {
    return std::round(px * 2.0f) * 0.5f;
}

} // namespace

Metrics build(float dpi, float zoom) {
    Metrics m;
    const float factor = clampScale(dpi) * clampScale(zoom);
    m.factor = factor;

    for (int i = 0; i < kTypeCount; ++i) {
        m.type[i] = snap(kTypePx[i] * factor);
        m.line[i] = snap(kTypePx[i] * kLineRatio[i] * factor);
    }
    for (int i = 0; i < kSpaceCount; ++i) m.space[i] = snap(kSpacePx[i] * factor);
    for (int i = 0; i < kRadiusCount; ++i) {
        m.radius[i] = (i == static_cast<int>(Radius::Pill)) ? 999.0f * factor
                                                            : snap(kRadiusPx[i] * factor);
    }

    // A control is one line of body text plus 8 px above and below: 30.5 px at
    // 100%, which is what a desktop button measures.
    m.controlH = snap(m.line[static_cast<int>(Type::Body)] +
                      2.0f * kSpacePx[static_cast<int>(Space::Xs)] * factor);
    m.rowH = snap(m.line[static_cast<int>(Type::Body)] +
                  kSpacePx[static_cast<int>(Space::Xs)] * factor);
    m.iconSize = snap(m.type[static_cast<int>(Type::Body)] * 1.15f);

    // Never below one physical pixel, never a fraction: a 0.7 px separator does
    // not render and leaves a gap where a border should be.
    m.hairline = std::max(1.0f, std::floor(factor));

    m.pagePad = snap(16.0f * factor);
    m.gutter = snap(12.0f * factor);
    // A column scales with the factor: at 150% the window shows the same number of
    // characters per line, which is the point of a reading measure. 900 px of the
    // 16 px monospace face is about 92 columns -- the width the TUI was designed
    // for, so a table that fits in the terminal fits here too.
    m.readingMax = snap(900.0f * factor);
    m.dockW = snap(340.0f * factor);
    m.scrollbar = std::max(6.0f, snap(10.0f * factor));
    return m;
}

float typePx(const Metrics& m, Type t) {
    return m.type[typeIndex(t)];
}

float linePx(const Metrics& m, Type t) {
    return m.line[typeIndex(t)];
}

float spacePx(const Metrics& m, Space s) {
    return m.space[spaceIndex(s)];
}

float radiusPx(const Metrics& m, Radius r) {
    return m.radius[radiusIndex(r)];
}

} // namespace tokens
} // namespace studio
