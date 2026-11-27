// proJV studio -- selection arithmetic on rendered lines (see text_runs.h).
#include "design/text_runs.h"

#include <algorithm>
#include <cstddef>

namespace studio {
namespace text_runs {
namespace {

size_t cpLen(const std::string& s, size_t i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c >= 0xF0) return 4;
    if (c >= 0xE0) return 3;
    if (c >= 0xC0) return 2;
    return 1;
}

} // namespace

int charCount(const std::string& utf8) {
    int n = 0;
    size_t i = 0;
    while (i < utf8.size()) {
        i += cpLen(utf8, i);
        ++n;
    }
    return n;
}

int columnAt(const std::vector<float>& charX, float dx) {
    if (charX.size() <= 1) return 0;
    // charX[i] is the left edge of character i; the LAST entry is the line's total
    // width. upper_bound finds the first edge past the pointer, and one step back is
    // the character the pointer sits on -- the same "cell" rule a terminal uses,
    // but with the font's own advances instead of a guessed width.
    const auto it = std::upper_bound(charX.begin(), charX.end(), dx);
    int idx = static_cast<int>(it - charX.begin()) - 1;
    const int last = static_cast<int>(charX.size()) - 1;
    if (idx < 0) idx = 0;
    if (idx > last) idx = last;
    return idx;
}

std::string sliceChars(const std::string& utf8, int c0, int c1) {
    if (c0 < 0) c0 = 0;
    if (c1 < c0) c1 = c0;
    size_t b0 = utf8.size();
    size_t b1 = utf8.size();
    int idx = 0;
    size_t i = 0;
    while (i <= utf8.size()) {
        if (idx == c0 && b0 == utf8.size()) b0 = i;
        if (idx == c1) {
            b1 = i;
            break;
        }
        if (i == utf8.size()) break;
        i += cpLen(utf8, i);
        ++idx;
    }
    if (b1 < b0) b1 = b0;
    return utf8.substr(b0, b1 - b0);
}

std::string extract(const std::vector<Line>& lines, const Selection& sel) {
    if (sel.empty()) return std::string();
    int b0, l0, c0, b1, l1, c1;
    sel.ordered(&b0, &l0, &c0, &b1, &l1, &c1);

    std::string out;
    bool wroteAny = false;
    for (const Line& ln : lines) {
        if (!lineInRange(ln.block, ln.line, b0, l0, b1, l1)) continue;
        const int columns = charCount(ln.text);
        const bool isStart = (ln.block == b0 && ln.line == l0);
        const bool isEnd = (ln.block == b1 && ln.line == l1);
        const bool whole = !isStart && !isEnd;

        int from = isStart ? c0 : 0;
        int to = isEnd ? c1 : columns;
        if (whole) {
            from = 0;
            to = columns;
        }
        // An end line whose span is empty contributes nothing: a caret sitting at
        // column 0 must not leave a blank line at the bottom of the clipboard.
        if (!whole && to <= from) continue;

        // A new visual line means a new line in the text -- unless it is a soft wrap
        // of the previous one, in which case the sentence is one sentence.
        if (wroteAny && !ln.soft) out += '\n';
        out += sliceChars(ln.text, from, to);
        wroteAny = true;
    }
    return out;
}

} // namespace text_runs
} // namespace studio
