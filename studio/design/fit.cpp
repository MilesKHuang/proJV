// proJV studio -- text measuring and fitting (see fit.h).
#include "fit.h"

#include <algorithm>

namespace studio {
namespace fit {
namespace {

// The wide ranges that matter for a chat window: CJK, kana, Hangul, fullwidth
// forms, and the emoji blocks. A terminal treats these as two cells, so the two
// front-ends must agree on what "two columns" means.
bool inRange(unsigned int cp, unsigned int lo, unsigned int hi) {
    return cp >= lo && cp <= hi;
}

} // namespace

std::size_t codePointLen(const std::string& utf8, std::size_t i) {
    if (i >= utf8.size()) return 0;
    const unsigned char c = static_cast<unsigned char>(utf8[i]);
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1; // stray continuation byte: consume one and move on
}

unsigned int decode(const std::string& utf8, std::size_t i) {
    if (i >= utf8.size()) return 0xFFFD;
    const unsigned char c0 = static_cast<unsigned char>(utf8[i]);
    const std::size_t len = codePointLen(utf8, i);
    if (len == 1) return c0 < 0x80 ? c0 : 0xFFFD;
    if (i + len > utf8.size()) return 0xFFFD;
    unsigned int cp = 0;
    if (len == 2) {
        cp = static_cast<unsigned int>(c0 & 0x1F);
    } else if (len == 3) {
        cp = static_cast<unsigned int>(c0 & 0x0F);
    } else {
        cp = static_cast<unsigned int>(c0 & 0x07);
    }
    for (std::size_t k = 1; k < len; ++k) {
        const unsigned char c = static_cast<unsigned char>(utf8[i + k]);
        if ((c & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | static_cast<unsigned int>(c & 0x3F);
    }
    return cp;
}

bool isWide(unsigned int cp) {
    return inRange(cp, 0x1100, 0x115F) ||   // Hangul Jamo
           inRange(cp, 0x2E80, 0x303E) ||   // CJK radicals, punctuation
           inRange(cp, 0x3041, 0x33FF) ||   // kana, Hangul compat, CJK compat
           inRange(cp, 0x3400, 0x4DBF) ||   // CJK ext A
           inRange(cp, 0x4E00, 0x9FFF) ||   // CJK unified
           inRange(cp, 0xA000, 0xA4CF) ||   // Yi
           inRange(cp, 0xAC00, 0xD7A3) ||   // Hangul syllables
           inRange(cp, 0xF900, 0xFAFF) ||   // compatibility ideographs
           inRange(cp, 0xFE30, 0xFE6F) ||   // CJK compatibility forms
           inRange(cp, 0xFF00, 0xFF60) ||   // fullwidth forms
           inRange(cp, 0xFFE0, 0xFFE6) ||   // fullwidth signs
           inRange(cp, 0x1F300, 0x1F64F) || // emoji (pictographs)
           inRange(cp, 0x1F900, 0x1F9FF) || // emoji (supplemental)
           inRange(cp, 0x20000, 0x3FFFD);   // CJK ext B and beyond
}

int displayColumns(const std::string& utf8) {
    int cols = 0;
    std::size_t i = 0;
    while (i < utf8.size()) {
        const std::size_t len = codePointLen(utf8, i);
        if (len == 0) break;
        cols += isWide(decode(utf8, i)) ? 2 : 1;
        i += len;
    }
    return cols;
}

std::string prefixThatFits(const std::string& text, float width, const Measure& measure) {
    if (text.empty() || width <= 0.0f || !measure) return std::string();
    if (measure(text) <= width) return text;

    // Grow the prefix one code point at a time. Measuring the whole candidate each
    // step is O(n^2) in code points, but a header label is tens of characters and
    // the alternative (summing per-glyph widths) is wrong for kerning and for the
    // fallback face used by the CJK glyphs.
    std::string best;
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t len = codePointLen(text, i);
        if (len == 0) break;
        const std::string candidate = text.substr(0, i + len);
        if (measure(candidate) > width) break;
        best = candidate;
        i += len;
    }
    return best;
}

std::string ellipsize(const std::string& text, float width, const Measure& measure) {
    if (text.empty()) return std::string();
    if (width <= 0.0f || !measure) return std::string();
    if (measure(text) <= width) return text;

    const std::string marker = "...";
    const float markerW = measure(marker);
    // The marker alone does not fit: it still has to be drawn, because an empty
    // box where a label belongs looks like a rendering bug.
    if (markerW >= width) return marker;

    std::string head = prefixThatFits(text, width - markerW, measure);
    // Do not leave a trailing space before the marker ("Save & Conn ..." reads as
    // a word that lost its ending rather than as a truncation).
    while (!head.empty() && head.back() == ' ') head.pop_back();
    if (head.empty()) return marker;
    return head + marker;
}

int bestTier(const std::string& text, float width, const float* sizes, int count,
             const UnitMeasure& unitWidth) {
    if (count <= 0 || sizes == nullptr) return 0;
    if (!unitWidth) return count - 1;
    const float unit = unitWidth(text);
    for (int i = 0; i < count; ++i) {
        if (sizes[i] <= 0.0f) continue;
        if (unit * sizes[i] <= width) return i;
    }
    return count - 1;
}

} // namespace fit
} // namespace studio
