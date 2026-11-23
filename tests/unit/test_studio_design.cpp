// proJV tests -- the studio's design system (design tokens, layout, text fitting).
//
// These three modules are deliberately ImGui-free, so the geometry the whole GUI
// is built from can be pinned without a GPU, a window or a frame. Every resizing
// bug the old window had ("the bands are a frame behind", "the dock is 36
// characters wide") was a geometry bug; this file is what stops those coming back.
#include "doctest.h"

#include "design/actions.h"
#include "design/fit.h"
#include "design/layout.h"
#include "design/tokens.h"

#include <algorithm>
#include <set>
#include <string>

using studio::fit::Measure;
using studio::fit::UnitMeasure;
using studio::layout::Node;
using studio::layout::Rect;
using studio::tokens::Metrics;

namespace {

// A fake font: 8 px per ASCII byte at size 16, 16 px per CJK code point. The real
// measurer is ImGui's; these numbers are chosen so every expectation below is an
// exact integer.
float fakeMeasure(const std::string& s, float size = 16.0f) {
    const int cols = studio::fit::displayColumns(s);
    return static_cast<float>(cols) * 8.0f * (size / 16.0f);
}

Measure measurer() {
    return [](const std::string& s) { return fakeMeasure(s); };
}

UnitMeasure unitMeasurer() {
    return [](const std::string& s) { return fakeMeasure(s, 1.0f); };
}

bool near(float a, float b) {
    const float d = a - b;
    return (d < 0.01f && d > -0.01f);
}

} // namespace

// ---- tokens --------------------------------------------------------------

TEST_CASE("tokens: the type ladder is ordered and Body is the reading size") {
    const Metrics m = studio::tokens::build(1.0f);
    const float display = studio::tokens::typePx(m, studio::tokens::Type::Display);
    const float title = studio::tokens::typePx(m, studio::tokens::Type::Title);
    const float heading = studio::tokens::typePx(m, studio::tokens::Type::Heading);
    const float body = studio::tokens::typePx(m, studio::tokens::Type::Body);
    const float small = studio::tokens::typePx(m, studio::tokens::Type::Small);
    const float caption = studio::tokens::typePx(m, studio::tokens::Type::Caption);

    CHECK(display > title);
    CHECK(title > heading);
    CHECK(heading > body);
    CHECK(body > small);
    CHECK(small > caption);
    // Body is the tier a chat message is read at: 16 px at 100%. It was 14.5 in
    // the first pass, which on a 4K panel at 100% was too small to read comfortably.
    CHECK(near(body, 16.0f));
    // The line height leaves room for descenders and for a 4 px leading.
    CHECK(studio::tokens::linePx(m, studio::tokens::Type::Body) > body);
}

TEST_CASE("tokens: every size follows the scale, and no hairline drops below 1 px") {
    for (float scale : {0.75f, 1.0f, 1.25f, 1.5f, 2.0f}) {
        const Metrics m = studio::tokens::build(scale);
        CHECK(near(m.factor, scale));
        CHECK(m.hairline >= 1.0f);
        CHECK(m.controlH > m.line[static_cast<int>(studio::tokens::Type::Body)]);
        // The page margin and the gutter are a 4 px rhythm, never a fraction of one.
        CHECK(m.space[static_cast<int>(studio::tokens::Space::Xs)] >= 0.0f);
        for (int i = 1; i < studio::tokens::kSpaceCount; ++i) {
            CHECK(m.space[i] > m.space[i - 1]);
        }
    }
}

TEST_CASE("tokens: a nonsense scale is clamped instead of producing a zero-size window") {
    const Metrics zero = studio::tokens::build(0.0f);
    CHECK(zero.factor >= 0.5f);
    CHECK(zero.controlH > 0.0f);

    const Metrics huge = studio::tokens::build(12.0f);
    CHECK(huge.factor <= 4.0f);

    const Metrics zoomed = studio::tokens::build(1.0f, 1.5f);
    CHECK(near(zoomed.factor, 1.5f));
    CHECK(zoomed.type[static_cast<int>(studio::tokens::Type::Body)] >
          studio::tokens::typePx(studio::tokens::build(1.0f),
                                 studio::tokens::Type::Body));
}

// ---- layout --------------------------------------------------------------

TEST_CASE("layout: a row with a growing child fills its parent") {
    Node root = Node::row(0.0f);
    root.add(Node::none().width(100.0f));
    root.add(Node::none().wGrow(1.0f));
    root.add(Node::none().width(50.0f));
    studio::layout::solve(root, Rect{0.0f, 0.0f, 400.0f, 100.0f});

    CHECK(near(root.children[0].rect.w, 100.0f));
    CHECK(near(root.children[1].rect.w, 250.0f));
    CHECK(near(root.children[2].rect.w, 50.0f));
    CHECK(near(root.children[2].rect.right(), 400.0f));
}

TEST_CASE("layout: padding and gaps are subtracted before the grow share is split") {
    Node root = Node::row(10.0f);
    root.padding(20.0f);
    root.add(Node::none().width(60.0f));
    root.add(Node::none().wGrow(1.0f));
    studio::layout::solve(root, Rect{0.0f, 0.0f, 300.0f, 100.0f});

    // 300 - 2*20 padding - 10 gap - 60 fixed = 190 for the growing child.
    CHECK(near(root.children[0].rect.x, 20.0f));
    CHECK(near(root.children[1].rect.x, 90.0f));
    CHECK(near(root.children[1].rect.w, 190.0f));
}

TEST_CASE("layout: two growing children split the free space by weight") {
    Node root = Node::row(0.0f);
    root.add(Node::none().wGrow(1.0f));
    root.add(Node::none().wGrow(3.0f));
    studio::layout::solve(root, Rect{0.0f, 0.0f, 400.0f, 50.0f});

    CHECK(near(root.children[0].rect.w, 100.0f));
    CHECK(near(root.children[1].rect.w, 300.0f));
}

TEST_CASE("layout: a column stack is placed top to bottom and never overlaps") {
    Node root = Node::col(8.0f);
    root.add(Node::none().height(40.0f));
    root.add(Node::none().hGrow(1.0f));
    root.add(Node::none().height(30.0f));
    studio::layout::solve(root, Rect{0.0f, 0.0f, 500.0f, 200.0f});

    CHECK(near(root.children[0].rect.y, 0.0f));
    CHECK(near(root.children[1].rect.y, 48.0f));
    CHECK(near(root.children[2].rect.y, 48.0f + root.children[1].rect.h + 8.0f));
    CHECK(near(root.children[2].rect.bottom(), 200.0f));
    CHECK(root.children[0].rect.bottom() <= root.children[1].rect.y);
}

TEST_CASE("layout: a percent child takes its share of the parent's inner size") {
    Node root = Node::row(0.0f);
    root.padding(10.0f);
    root.add(Node::none().wPercent(0.25f));
    root.add(Node::none().wPercent(0.75f));
    studio::layout::solve(root, Rect{0.0f, 0.0f, 420.0f, 100.0f});

    CHECK(near(root.children[0].rect.w, 100.0f));
    CHECK(near(root.children[1].rect.w, 300.0f));
}

TEST_CASE("layout: min and max clamp a child instead of letting it overflow") {
    Node root = Node::row(0.0f);
    root.add(Node::none().wGrow(1.0f).maxsize(120.0f, 0.0f));
    root.add(Node::none().wGrow(1.0f));
    studio::layout::solve(root, Rect{0.0f, 0.0f, 400.0f, 60.0f});

    CHECK(near(root.children[0].rect.w, 120.0f));
    CHECK(root.children[1].rect.w >= 279.0f);
}

TEST_CASE("layout: cross-axis stretch fills, start keeps the content size") {
    Node stretched = Node::col(0.0f);
    stretched.add(Node::none().height(20.0f));
    studio::layout::solve(stretched, Rect{0.0f, 0.0f, 300.0f, 100.0f});
    CHECK(near(stretched.children[0].rect.w, 300.0f));

    Node started = Node::col(0.0f);
    started.crossAlign(studio::layout::Align::Start);
    started.add(Node::none().width(80.0f).height(20.0f));
    studio::layout::solve(started, Rect{0.0f, 0.0f, 300.0f, 100.0f});
    CHECK(near(started.children[0].rect.w, 80.0f));
    CHECK(near(started.children[0].rect.x, 0.0f));
}

TEST_CASE("layout: justify places the run and space-between pushes it apart") {
    Node endAligned = Node::row(0.0f);
    endAligned.mainAlign(studio::layout::Justify::End);
    endAligned.add(Node::none().width(50.0f));
    studio::layout::solve(endAligned, Rect{0.0f, 0.0f, 200.0f, 40.0f});
    CHECK(near(endAligned.children[0].rect.x, 150.0f));

    Node spread = Node::row(0.0f);
    spread.mainAlign(studio::layout::Justify::SpaceBetween);
    spread.add(Node::none().width(50.0f));
    spread.add(Node::none().width(50.0f));
    spread.add(Node::none().width(50.0f));
    studio::layout::solve(spread, Rect{0.0f, 0.0f, 250.0f, 40.0f});
    CHECK(near(spread.children[0].rect.x, 0.0f));
    CHECK(near(spread.children[1].rect.x, 100.0f));
    CHECK(near(spread.children[2].rect.x, 200.0f));
}

TEST_CASE("layout: a child never reaches outside its parent") {
    Node inner = Node::row(0.0f);
    inner.add(Node::none().width(100.0f));
    inner.add(Node::none().wGrow(1.0f));
    Node root = Node::col(0.0f);
    root.add(inner.height(30.0f));
    studio::layout::solve(root, Rect{10.0f, 20.0f, 200.0f, 100.0f});

    const Rect& row = root.children[0].rect;
    CHECK(row.x >= 10.0f);
    CHECK(row.right() <= 210.01f);
    CHECK(row.y >= 20.0f);
    CHECK(row.bottom() <= 120.01f);
}

// ---- text fitting --------------------------------------------------------

TEST_CASE("fit: display columns count a CJK glyph as two") {
    CHECK(studio::fit::displayColumns("abc") == 3);
    CHECK(studio::fit::displayColumns("\xe4\xbd\xa0\xe5\xa5\xbd") == 4); // "ni hao"
    CHECK(studio::fit::displayColumns("a\xe4\xbd\xa0") == 3);
    CHECK(studio::fit::displayColumns("") == 0);
}

TEST_CASE("fit: code points are measured whole, never split") {
    const std::string s = "a\xe4\xbd\xa0";
    CHECK(studio::fit::codePointLen(s, 0) == 1);
    CHECK(studio::fit::codePointLen(s, 1) == 3);
    CHECK(studio::fit::decode(s, 1) == 0x4F60u);
}

TEST_CASE("fit: ellipsize returns the text unchanged when it fits") {
    CHECK(studio::fit::ellipsize("Save", 100.0f, measurer()) == "Save");
    CHECK(studio::fit::ellipsize("", 100.0f, measurer()) == "");
}

TEST_CASE("fit: ellipsize cuts on a code point boundary and keeps the marker") {
    // 32 px for "abcd", and 24 px for the marker itself: room for one character.
    const std::string out = studio::fit::ellipsize("abcdefgh", 32.0f, measurer());
    CHECK(out == "a...");
    CHECK(out.substr(out.size() - 3) == "...");
    // A CJK string must not be cut in the middle of a 3-byte sequence.
    const std::string cjk = studio::fit::ellipsize("\xe4\xbd\xa0\xe5\xa5\xbd\xe4\xb8\x96\xe7\x95\x8c",
                                                  40.0f, measurer());
    CHECK(studio::fit::displayColumns(cjk) <= 5);
    CHECK(cjk.size() >= 3);
}

TEST_CASE("fit: a box narrower than the marker still shows the marker") {
    CHECK(studio::fit::ellipsize("abcdef", 4.0f, measurer()) == "...");
    CHECK(studio::fit::ellipsize("abcdef", 0.0f, measurer()) == "");
}

TEST_CASE("fit: bestTier picks the largest tier that fits") {
    const float sizes[3] = {32.0f, 16.0f, 8.0f};
    // "abcd" is 4 columns = 0.5 unit per px of font size * 4 = 2 px per 1 px size.
    const UnitMeasure unit = unitMeasurer();
    CHECK(studio::fit::bestTier("abcd", 200.0f, sizes, 3, unit) == 0);
    CHECK(studio::fit::bestTier("abcd", 40.0f, sizes, 3, unit) == 1);
    CHECK(studio::fit::bestTier("abcd", 10.0f, sizes, 3, unit) == 2);
    // Nothing fits: the smallest tier is still the answer, never an out-of-range one.
    CHECK(studio::fit::bestTier("abcd", 1.0f, sizes, 3, unit) == 2);
}

// ---- the multi-line composer --------------------------------------------

TEST_CASE("fit: wrappedRowCount counts the rows a multiline field needs") {
    // The fake face is 8 px per column at 16 px, so a 64 px box holds 8 columns.
    CHECK(studio::fit::wrappedRowCount("", 64.0f, measurer()) == 1);
    CHECK(studio::fit::wrappedRowCount("abcd", 64.0f, measurer()) == 1);
    // Exactly at the edge is still one row, never a phantom second one.
    CHECK(studio::fit::wrappedRowCount("abcdefgh", 64.0f, measurer()) == 1);
    CHECK(studio::fit::wrappedRowCount("abcdefghi", 64.0f, measurer()) == 2);
    CHECK(studio::fit::wrappedRowCount("abcdefghijklmnopq", 64.0f, measurer()) == 3);
    // Every explicit newline starts a row, including a trailing one (the caret is
    // on that row: a composer that hides it swallows the next character).
    CHECK(studio::fit::wrappedRowCount("a\nb", 64.0f, measurer()) == 2);
    CHECK(studio::fit::wrappedRowCount("a\n", 64.0f, measurer()) == 2);
    CHECK(studio::fit::wrappedRowCount("\n\n", 64.0f, measurer()) == 3);
    // A row that wraps AND a newline: 9 columns = 2 rows, then one more line.
    CHECK(studio::fit::wrappedRowCount("abcdefghi\nz", 64.0f, measurer()) == 3);
    // An unmeasured box (0 px wide on the first frame) is one row per line, and a
    // character wider than the box still needs a whole row.
    CHECK(studio::fit::wrappedRowCount("abc\ndef", 0.0f, measurer()) == 2);
    CHECK(studio::fit::wrappedRowCount("ab", 8.0f, measurer()) == 2);
    // CJK counts as two columns here too: 4 wide glyphs = 64 px = one row.
    CHECK(studio::fit::wrappedRowCount("\xe4\xbd\xa0\xe5\xa5\xbd\xe4\xb8\x96\xe7\x95\x8c", 64.0f,
                                       measurer()) == 1);
}

// ---- the header's button table ------------------------------------------

TEST_CASE("actions: every documented key has exactly one header button") {
    // The reference manual's key table (docs/*/user-guide/08-reference.md) and the
    // buttons must be the same set: a key the product answers to but the header
    // cannot show is the report this table was written for.
    std::set<std::string> documented = {"F1", "F2", "F3", "F4", "F5",
                                       "F6", "F7", "F8", "F9", "F10", "Tab"};
    std::set<std::string> shown;
    for (const auto& item : studio::actions::header()) {
        shown.insert(item.shortcut);
        CHECK(item.label != nullptr);
        CHECK(std::string(item.label).size() > 0);
    }
    CHECK(shown.size() == studio::actions::header().size()); // no key documented twice
    CHECK(shown == documented);
}

TEST_CASE("actions: a toggle and the role chip say what state they are in") {
    using studio::actions::Id;
    studio::actions::State st;
    st.dock = true;
    st.reasoning = false;
    st.role = "monica";
    st.promptFiles = 3;
    CHECK(studio::actions::label(Id::Dock, st) == "Dock on");
    CHECK(studio::actions::label(Id::Reasoning, st) == "Reasoning off");
    CHECK(studio::actions::label(Id::Role, st) == "Role: monica");
    // No prompt file installed: the chip still has to be clickable and named.
    studio::actions::State empty;
    empty.role.clear();
    empty.promptFiles = 0;
    CHECK(studio::actions::label(Id::Role, empty) == "Role");
    st.dock = false;
    st.reasoning = true;
    CHECK(studio::actions::label(Id::Dock, st) == "Dock off");
    CHECK(studio::actions::label(Id::Reasoning, st) == "Reasoning on");
    // Static labels come from the table, so the header cannot invent a second word.
    CHECK(studio::actions::label(Id::Sessions, st) == "Sessions");
    // A tooltip leads with the shortcut the button stands for.
    CHECK(studio::actions::tooltip(Id::Dock, st).rfind("F9", 0) == 0);
    CHECK(studio::actions::tooltip(Id::Role, st).rfind("Tab", 0) == 0);
}

TEST_CASE("actions: pack wraps instead of dropping a button") {
    // Five 100 px controls, 8 px gaps: two fit in 250 px, the rest follow.
    const std::vector<float> widths = {100.0f, 100.0f, 100.0f, 100.0f, 100.0f};
    const auto rows = studio::actions::pack(widths, 8.0f, 250.0f, 250.0f);
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].size() == 2); // 100 + 8 + 100 = 208 <= 250, a third would not fit
    CHECK(rows[1].size() == 2);
    CHECK(rows[2].size() == 1);
    // Every control is used exactly once, in order: nothing lost, nothing moved.
    size_t seen = 0;
    for (const auto& row : rows) {
        for (int idx : row) CHECK(idx == static_cast<int>(seen++));
    }
    CHECK(seen == widths.size());
}

TEST_CASE("actions: row 0 is narrower than the rows under it") {
    const std::vector<float> widths = {100.0f, 100.0f, 100.0f};
    // The title shares row 0, so only one control fits there; the rest use the
    // full width below it.
    const auto rows = studio::actions::pack(widths, 8.0f, 120.0f, 250.0f);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].size() == 1);
    CHECK(rows[1].size() == 2);
}

TEST_CASE("actions: a control wider than the window still gets a row") {
    const std::vector<float> widths = {400.0f, 50.0f};
    const auto rows = studio::actions::pack(widths, 8.0f, 100.0f, 100.0f);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].size() == 1);
    CHECK(rows[0][0] == 0);
    CHECK(rows[1].size() == 1);
    CHECK(rows[1][0] == 1);
    // No controls at all is a valid answer, not a crash.
    CHECK(studio::actions::pack({}, 8.0f, 100.0f, 100.0f).empty());
}
