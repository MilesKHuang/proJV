// proJV tests -- selection arithmetic on RENDERED lines.
//
// The window that draws the text cannot run here (it needs a font and a frame), but
// the arithmetic that turns a drag into copied characters can, and it is exactly
// where "the highlighted area does not match the copied text" lived:
//
//   - x -> column used to be "columns x width('M')", which is wrong for CJK (a
//     Chinese glyph is its own full-width box, not two Latin cells). Here the
//     per-character pixel offsets are handed in explicitly, so the column lookup
//     is the same rule the drawing uses.
//   - the copy used to walk an append-only cache, so a folded row or a stale
//     streamed block leaked in with no highlight. `extract` only ever sees the
//     lines it is given -- one frame's drawn lines -- so it cannot.
#include "doctest.h"

#include <string>
#include <vector>

#include "design/text_runs.h"
#include "design/text_select.h"

using studio::Selection;
using studio::text_runs::charCount;
using studio::text_runs::columnAt;
using studio::text_runs::extract;
using studio::text_runs::Line;
using studio::text_runs::sliceChars;

namespace {

// Build a Line as the surface does: charX has one entry per character plus a
// trailing total. Advances here are made up (10 px per char, 20 for a wide one) but
// they stand for "whatever the font measured" -- the point is the arithmetic reads
// THEM, not a global guess.
Line makeLine(int block, int line, const std::string& text, bool soft = false) {
    Line ln;
    ln.block = block;
    ln.line = line;
    ln.text = text;
    ln.soft = soft;
    auto isWide = [](const std::string& s, size_t i) {
        return static_cast<unsigned char>(s[i]) >= 0xE0;
    };
    float x = 0.0f;
    ln.charX.push_back(0.0f);
    size_t i = 0;
    while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        size_t len = 1;
        if (c >= 0xF0)
            len = 4;
        else if (c >= 0xE0)
            len = 3;
        else if (c >= 0xC0)
            len = 2;
        x += isWide(text, i) ? 20.0f : 10.0f;
        ln.charX.push_back(x);
        i += len;
    }
    return ln;
}

Selection sel(int b0, int l0, int c0, int b1, int l1, int c1) {
    Selection s;
    s.blockA = b0;
    s.lineA = l0;
    s.colA = c0;
    s.blockB = b1;
    s.lineB = l1;
    s.colB = c1;
    s.active = true;
    return s;
}

} // namespace

TEST_CASE("text_runs: charCount counts code points, not bytes") {
    CHECK(charCount("") == 0);
    CHECK(charCount("abc") == 3);
    CHECK(charCount("\xe4\xb8\xad\xe6\x96\x87") == 2); // two CJK characters
}

TEST_CASE("text_runs: columnAt reads the measured offsets, not a fixed cell") {
    // "a中b": a is 10 px, 中 is 20, b is 10. Offsets: [0, 10, 30, 40].
    const std::string s = "a\xe4\xb8\xad"
                          "b";
    const Line ln = makeLine(0, 0, s);
    REQUIRE(ln.charX.size() == 4);
    CHECK(columnAt(ln.charX, 0.0f) == 0);
    CHECK(columnAt(ln.charX, 5.0f) == 0);   // inside 'a'
    CHECK(columnAt(ln.charX, 10.0f) == 1);  // left edge of 中
    CHECK(columnAt(ln.charX, 25.0f) == 1);  // inside 中
    CHECK(columnAt(ln.charX, 30.0f) == 2);  // left edge of 'b'
    CHECK(columnAt(ln.charX, 39.0f) == 2);  // inside 'b'
    CHECK(columnAt(ln.charX, 400.0f) == 3); // past the end clamps to the last index
    CHECK(columnAt(ln.charX, -10.0f) == 0);
}

TEST_CASE("text_runs: sliceChars is code-point exact") {
    const std::string s = "a\xe4\xb8\xad"
                          "b"; // a, 中, b
    CHECK(sliceChars(s, 0, 1) == "a");
    CHECK(sliceChars(s, 1, 2) == "\xe4\xb8\xad");
    CHECK(sliceChars(s, 0, 2) == "a\xe4\xb8\xad");
    CHECK(sliceChars(s, 0, 3) == s);
    CHECK(sliceChars(s, 2, 1) == ""); // reversed -> empty, never a bad substr
    CHECK(sliceChars(s, 0, 99) == s); // clamped
}

TEST_CASE("text_runs: extract copies a single line span") {
    const std::vector<Line> lines = {makeLine(0, 0, "hello world")};
    // Columns 6..11 -> "world".
    CHECK(extract(lines, sel(0, 0, 6, 0, 0, 11)) == "world");
    // A backwards drag is normalised.
    CHECK(extract(lines, sel(0, 0, 11, 0, 0, 6)) == "world");
}

TEST_CASE("text_runs: extract keeps real newlines but joins soft wraps") {
    // Two visual lines, both a soft wrap of one paragraph.
    const std::vector<Line> soft = {makeLine(0, 0, "one two ", true), makeLine(0, 1, "three", true)};
    CHECK(extract(soft, sel(0, 0, 0, 0, 1, 5)) == "one two three");

    // Two real lines (soft == false on the second): a newline is kept.
    const std::vector<Line> hard = {makeLine(0, 0, "one", false), makeLine(0, 1, "two", false)};
    CHECK(extract(hard, sel(0, 0, 0, 0, 1, 3)) == "one\ntwo");
}

TEST_CASE("text_runs: extract cannot leak a line that was not passed in") {
    // The old bug: the clipboard walked an append-only cache, so a folded row or a
    // stale streamed block (block 3 here) was pasted with no highlight on screen.
    // extract only sees the lines it is given -- one frame's drawn lines -- so the
    // selection stops at the last line actually present.
    const std::vector<Line> drawn = {makeLine(0, 0, "visible one"), makeLine(1, 0, "visible two")};
    // A selection that claims to reach block 3, which is NOT in `drawn`.
    const std::string got = extract(drawn, sel(0, 0, 0, 3, 0, 5));
    CHECK(got == "visible one\nvisible two");
    CHECK(got.find("hidden") == std::string::npos);
}

TEST_CASE("text_runs: a caret at column 0 of the last line adds no blank line") {
    // Dragging down to the very start of the next line must not leave a trailing
    // empty line in the clipboard (an older bug appended the span unconditionally).
    const std::vector<Line> lines = {makeLine(0, 0, "abc"), makeLine(0, 1, "def")};
    CHECK(extract(lines, sel(0, 0, 0, 0, 1, 0)) == "abc");
}

TEST_CASE("text_runs: a CJK span copies the glyphs under the dragged pixels") {
    // "中文测试": every char is 20 px. Dragging from x=0 to x=40 covers exactly the
    // first two glyphs. With the old "2 x width('M')" model this drifted; with the
    // measured offsets it is exact.
    const std::string s = "\xe4\xb8\xad\xe6\x96\x87\xe6\xb5\x8b\xe8\xaf\x95"; // 中文测试
    const Line ln = makeLine(0, 0, s);
    REQUIRE(ln.charX.size() == 5);
    const int endCol = columnAt(ln.charX, 40.0f);
    CHECK(endCol == 2);
    CHECK(extract({ln}, sel(0, 0, 0, 0, 0, endCol)) == "\xe4\xb8\xad\xe6\x96\x87"); // 中文
}
