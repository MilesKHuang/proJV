// proJV tests -- what a text selection covers.
//
// The chat surface that draws the text cannot be tested here (it needs a font, a
// draw list and a frame), but the arithmetic that decides WHICH characters a drag
// selected can. Every bug in this file is invisible in a screenshot: the highlight
// looks right while the clipboard is scrambled, or a drag downwards copies the
// transcript backwards. These are the rules that stop it.
#include "doctest.h"

#include <vector>

#include "design/text_select.h"

using studio::columnSpan;
using studio::lineInRange;
using studio::Selection;

namespace {

// One line of a block, addressed the way the surface addresses it.
struct Cand {
    int block;
    int line;
};

std::vector<Cand> covered(const Selection& sel, const std::vector<Cand>& lines) {
    int b0, l0, c0, b1, l1, c1;
    sel.ordered(&b0, &l0, &c0, &b1, &l1, &c1);
    std::vector<Cand> out;
    for (const auto& c : lines)
        if (lineInRange(c.block, c.line, b0, l0, b1, l1)) out.push_back(c);
    return out;
}

} // namespace

TEST_CASE("selection: a forward drag is already in order") {
    Selection s;
    s.blockA = 1;
    s.lineA = 2;
    s.colA = 3;
    s.blockB = 4;
    s.lineB = 0;
    s.colB = 7;
    int b0, l0, c0, b1, l1, c1;
    s.ordered(&b0, &l0, &c0, &b1, &l1, &c1);
    CHECK(b0 == 1);
    CHECK(l0 == 2);
    CHECK(c0 == 3);
    CHECK(b1 == 4);
    CHECK(l1 == 0);
    CHECK(c1 == 7);
}

TEST_CASE("selection: a backward drag is normalised, never reversed") {
    Selection s;
    s.blockA = 4; // the caret is ABOVE the anchor
    s.lineA = 0;
    s.colA = 7;
    s.blockB = 1;
    s.lineB = 2;
    s.colB = 3;
    int b0, l0, c0, b1, l1, c1;
    s.ordered(&b0, &l0, &c0, &b1, &l1, &c1);
    CHECK(b0 == 1);
    CHECK(l0 == 2);
    CHECK(c0 == 3);
    CHECK(b1 == 4);
    CHECK(l1 == 0);
    CHECK(c1 == 7);
}

TEST_CASE("selection: a drag inside one line keeps its direction") {
    Selection s;
    s.blockA = s.blockB = 2;
    s.lineA = s.lineB = 1;
    s.colA = 9;
    s.colB = 4; // dragged leftwards within the same line
    int b0, l0, c0, b1, l1, c1;
    s.ordered(&b0, &l0, &c0, &b1, &l1, &c1);
    CHECK(c0 == 4);
    CHECK(c1 == 9);

    // A one-character caret (the double-click case) is still "forward", so the pair
    // is never swapped on the axis that matters.
    Selection w;
    w.blockA = w.blockB = 2;
    w.lineA = w.lineB = 1;
    w.colA = w.colB = 5;
    w.ordered(&b0, &l0, &c0, &b1, &l1, &c1);
    CHECK(c0 == 5);
    CHECK(c1 == 5);
}

TEST_CASE("selection: the covered lines are exactly the ones between the ends") {
    Selection s;
    s.blockA = 1;
    s.lineA = 1;
    s.blockB = 3;
    s.lineB = 0;
    const std::vector<Cand> all = {{0, 0}, {1, 0}, {1, 1}, {1, 2}, {2, 1}, {3, 0}, {3, 1}};
    const std::vector<Cand> got = covered(s, all);
    // The anchor's own line is inclusive, the caret's line is inclusive, and every
    // line of a block in between is covered whole -- {2,1} included, {1,0} and
    // {3,1} not.
    CHECK(got.size() == 4);
    CHECK(got[0].block == 1);
    CHECK(got[0].line == 1);
    CHECK(got[1].line == 2);
    CHECK(got[2].block == 2);
    CHECK(got[3].block == 3);
}

TEST_CASE("selection: the column span of a line is clipped at the ends only") {
    // Selection from block 1 line 1 col 2 to block 2 line 0 col 5.
    int from = -1, to = -1;

    // The anchor's line starts at the anchor column and runs to its own end.
    CHECK(columnSpan(1, 1, 20, 1, 1, 2, 2, 0, 5, &from, &to));
    CHECK(from == 2);
    CHECK(to == 20);

    // A line between the two ends is covered whole.
    CHECK(columnSpan(1, 2, 12, 1, 1, 2, 2, 0, 5, &from, &to));
    CHECK(from == 0);
    CHECK(to == 12);

    // The caret's line starts at 0 and ends at the caret column.
    CHECK(columnSpan(2, 0, 9, 1, 1, 2, 2, 0, 5, &from, &to));
    CHECK(from == 0);
    CHECK(to == 5);
}

TEST_CASE("selection: a line contributes nothing when the span is empty") {
    int from = -1, to = -1;
    // A caret sitting at column 0 of its line: the clipboard must not gain a blank
    // line for it, which is what appending the span unconditionally used to do.
    CHECK_FALSE(columnSpan(2, 0, 9, 1, 1, 2, 2, 0, 0, &from, &to));
    // An empty line inside the range is still empty.
    CHECK_FALSE(columnSpan(1, 2, 0, 1, 1, 2, 2, 0, 5, &from, &to));
    // A line outside the range never contributes, whatever its columns are.
    CHECK_FALSE(columnSpan(0, 0, 40, 1, 1, 2, 2, 0, 5, &from, &to));
    CHECK_FALSE(columnSpan(2, 3, 40, 1, 1, 2, 2, 0, 5, &from, &to));
}

TEST_CASE("selection: a backward drag copies forwards") {
    // The whole point of ordered(): the same drag made in either direction must
    // produce the same clipboard, or a reader who drags up loses their text.
    Selection fwd;
    fwd.blockA = 1;
    fwd.lineA = 1;
    fwd.colA = 2;
    fwd.blockB = 2;
    fwd.lineB = 0;
    fwd.colB = 5;

    Selection back;
    back.blockA = 2;
    back.lineA = 0;
    back.colA = 5;
    back.blockB = 1;
    back.lineB = 1;
    back.colB = 2;

    int b0, l0, c0, b1, l1, c1;
    fwd.ordered(&b0, &l0, &c0, &b1, &l1, &c1);
    int d0, m0, e0, d1, m1, e1;
    back.ordered(&d0, &m0, &e0, &d1, &m1, &e1);
    CHECK(b0 == d0);
    CHECK(l0 == m0);
    CHECK(c0 == e0);
    CHECK(b1 == d1);
    CHECK(l1 == m1);
    CHECK(c1 == e1);
}
