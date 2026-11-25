// proJV tests -- the studio's design system (design tokens, layout, text fitting).
//
// These three modules are deliberately ImGui-free, so the geometry the whole GUI
// is built from can be pinned without a GPU, a window or a frame. Every resizing
// bug the old window had ("the bands are a frame behind", "the dock is 36
// characters wide") was a geometry bug; this file is what stops those coming back.
#include "doctest.h"

#include "design/actions.h"
#include "design/fit.h"
#include "design/file_tree.h"
#include "design/gauges.h"
#include "design/layout.h"
#include "design/path_text.h"
#include "design/tokens.h"
#include "design/work_clock.h"
#include "platform/isystem_stats.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
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

// ---- the file tree's model ----------------------------------------------

namespace {

studio::ftree::Entry ent(const std::string& name, bool isDir) {
    studio::ftree::Entry e;
    e.name = name;
    e.path = "C:/x/" + name;
    e.isDir = isDir;
    return e;
}

std::vector<std::string> namesOf(std::vector<studio::ftree::Entry> entries) {
    studio::ftree::sortEntries(entries);
    std::vector<std::string> out;
    for (const auto& e : entries) out.push_back(e.name);
    return out;
}

// A scratch directory that cleans itself up, so the test exercises the real
// directory reader without inheriting state from a previous run.
struct Scratch {
    std::filesystem::path dir;
    Scratch() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        dir = std::filesystem::temp_directory_path() /
              ("projv_ftree_" + std::to_string(static_cast<long long>(stamp)));
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
    }
    ~Scratch() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

} // namespace

TEST_CASE("ftree: Explorer order is folders first, then numbers by value") {
    // The defect this rule exists to remove: plain dictionary order puts b10
    // before b2, and strands folders between files.
    const std::vector<std::string> got =
        namesOf({ent("b10.txt", false), ent("b2.txt", false), ent("README.md", false),
                 ent("sub", true), ent("Alpha", false)});
    const std::vector<std::string> want = {"sub", "Alpha", "b2.txt", "b10.txt", "README.md"};
    CHECK(got == want);
}

TEST_CASE("ftree: a number run compares as a number, not as text") {
    // "v2" < "v10" though '1' < '2' as text, and text sorts before a number run at
    // the same position ("a" < "a1").
    const std::vector<std::string> got =
        namesOf({ent("v10", false), ent("v2", false), ent("a1", false), ent("a", false)});
    CHECK(got == std::vector<std::string>{"a", "a1", "v2", "v10"});
}

TEST_CASE("ftree: listDirectory reads a real directory in the same order") {
    Scratch scratch;
    REQUIRE(!scratch.dir.empty());
    std::error_code ec;
    std::filesystem::create_directories(scratch.dir / "zeta", ec);
    for (const char* name : {"b10.txt", "b2.txt", "a.md"}) {
        std::ofstream(scratch.dir / name) << "x";
    }
    std::string err;
    const auto entries = studio::ftree::listDirectory(scratch.dir.string(), &err);
    CHECK(err.empty());
    REQUIRE(entries.size() == 4);
    CHECK(entries[0].name == "zeta");
    CHECK(entries[0].isDir);
    CHECK(entries[1].name == "a.md");
    CHECK(entries[2].name == "b2.txt");
    CHECK(entries[3].name == "b10.txt");
    // A path that is not a directory is an error string, never a crash and never
    // an exception.
    std::string missing;
    const auto none = studio::ftree::listDirectory((scratch.dir / "nope").string(), &missing);
    CHECK(none.empty());
    CHECK(!missing.empty());
}

TEST_CASE("ftree: expanding a folder adds rows at depth 1, collapsing removes them") {
    Scratch scratch;
    std::error_code ec;
    std::filesystem::create_directories(scratch.dir / "sub", ec);
    std::ofstream(scratch.dir / "sub" / "inner.txt") << "x";
    std::ofstream(scratch.dir / "top.txt") << "x";

    studio::ftree::Tree tree;
    std::string err;
    REQUIRE(tree.setRoot(scratch.dir.string(), &err));
    CHECK(err.empty());
    CHECK(tree.rows().size() == 2); // sub/ + top.txt, in that order
    CHECK(tree.rows()[0].isDir);

    tree.expand((scratch.dir / "sub").string());
    const auto rows = tree.rows();
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].name == "sub");
    CHECK(rows[0].expanded);
    CHECK(rows[1].name == "inner.txt");
    CHECK(rows[1].depth == 1);
    CHECK(rows[2].depth == 0);

    tree.collapse((scratch.dir / "sub").string());
    CHECK(tree.rows().size() == 2);
    // Toggling twice is the same as not touching it.
    tree.toggle((scratch.dir / "sub").string());
    CHECK(tree.rows().size() == 3);
    tree.toggle((scratch.dir / "sub").string());
    CHECK(tree.rows().size() == 2);
}

TEST_CASE("ftree: a root that is not a directory is refused with a reason") {
    studio::ftree::Tree tree;
    std::string err;
    CHECK_FALSE(tree.setRoot("", &err));
    CHECK_FALSE(err.empty());
    err.clear();
    CHECK_FALSE(tree.setRoot("C:/definitely/not/here/at/all", &err));
    CHECK(!err.empty());
    CHECK(tree.rows().empty());
}

// ---- the injection text --------------------------------------------------

TEST_CASE("path_text: only what would be ambiguous gets quotes") {
    CHECK(studio::path_text::payload("D:\\ws\\projV\\src\\main.py", false) ==
          "D:\\ws\\projV\\src\\main.py");
    CHECK(studio::path_text::payload("D:\\ws\\my project\\a b.md", false) ==
          "\"D:\\ws\\my project\\a b.md\"");
    // The `@` prefix goes OUTSIDE the quotes; inside them it would be part of the
    // file name the agent tries to open.
    CHECK(studio::path_text::payload("D:\\ws\\my project\\a b.md", true) ==
          "@\"D:\\ws\\my project\\a b.md\"");
    // A trailing backslash doubles: an unescaped one would eat the closing quote.
    CHECK(studio::path_text::quotePath("D:\\") == "\"D:\\\\\"");
    CHECK(studio::path_text::needsQuotes("D:\\") == true);
    CHECK(studio::path_text::needsQuotes("C:\\plain") == false);
    // Shell punctuation counts as unsafe too.
    CHECK(studio::path_text::needsQuotes("C:\\a&b") == true);
    CHECK(studio::path_text::needsQuotes("C:\\a)b") == true);
    CHECK(studio::path_text::payload("", false).empty());
    CHECK(studio::path_text::payload("", true).empty());
}

// ---- gauges --------------------------------------------------------------

TEST_CASE("gauges: sizes use binary units and stop at three of them") {
    CHECK(studio::gauges::humanBytes(0) == "0 KB");
    CHECK(studio::gauges::humanBytes(48ull * 1024ull) == "48 KB");
    CHECK(studio::gauges::humanBytes(212ull * 1024ull * 1024ull) == "212 MB");
    CHECK(studio::gauges::humanBytes(3ull * 1024ull * 1024ull * 1024ull) == "3.0 GB");
}

TEST_CASE("gauges: an unmeasurable value is a dash, never a zero") {
    CHECK(studio::gauges::percentLabel(std::nullopt) == "-");
    CHECK(studio::gauges::memoryLabel(std::nullopt) == "-");
    CHECK(studio::gauges::percentLabel(37.4) == "37%");
    CHECK(studio::gauges::percentLabel(140.0) == "100%");
    CHECK(studio::gauges::percentLabel(-5.0) == "0%");
    // Memory keeps one decimal: 0.6% of a 32 GB machine must not read as "1%".
    CHECK(studio::gauges::memoryLabel(0.62) == "0.6%");
}

TEST_CASE("gauges: a bar's ceiling scales with what is there") {
    CHECK(studio::gauges::fraction(50.0, 100.0) == doctest::Approx(0.5));
    CHECK(studio::gauges::fraction(150.0, 100.0) == doctest::Approx(1.0));
    CHECK(studio::gauges::fraction(5.0, 0.0) == doctest::Approx(0.0));
    CHECK(studio::gauges::ceilingFor(4.0, 6.0) == doctest::Approx(6.0));
    CHECK(studio::gauges::ceilingFor(9.0, 6.0) == doctest::Approx(9.0));
    // Never zero: a ceiling of 0 would divide in the caller.
    CHECK(studio::gauges::ceilingFor(0.0, 0.0) > 0.0);
}

// ---- the turn clock ------------------------------------------------------

TEST_CASE("work_clock: elapsed time from injected timestamps") {
    studio::work_clock::Clock c;
    CHECK(studio::work_clock::elapsedSeconds(c, 5000) == doctest::Approx(0.0));

    studio::work_clock::begin(c, 1000, 100);
    CHECK(c.running);
    // A second begin (a re-entrant tick) must not restart the turn.
    studio::work_clock::begin(c, 4000, 900);
    CHECK(studio::work_clock::elapsedSeconds(c, 3000) == doctest::Approx(2.0));
    // Clock skew (a smaller `now`) can never produce a negative duration.
    CHECK(studio::work_clock::elapsedSeconds(c, 0) == doctest::Approx(0.0));

    studio::work_clock::end(c);
    CHECK_FALSE(c.running);
    CHECK(studio::work_clock::elapsedSeconds(c, 9000) == doctest::Approx(0.0));
}

TEST_CASE("work_clock: the rate is output tokens over a sliding window") {
    using namespace studio::work_clock;
    Clock c;
    // Nothing recorded yet: no rate, not a division by zero.
    CHECK(tokensPerSecond(c, 1000) == doctest::Approx(0.0));

    begin(c, 1000, 0);
    // A prompt-sized count passed as the baseline is exactly the bug that made the
    // figure read "30000 tok/s": the rate only ever counts what was EMITTED.
    CHECK(tokensPerSecond(c, 1200) == doctest::Approx(0.0)); // one sample: no span

    // The window at 2400 ms still holds the turn's first sample (1000 ms, 0 tokens),
    // so the figure is 200 tokens over 1.4 s -- an average, not a burst.
    noteTokens(c, 1200, 0);
    noteTokens(c, 1400, 100);
    noteTokens(c, 2200, 200);
    CHECK(tokensPerSecond(c, 2200) == doctest::Approx(200.0 * 1000.0 / 1200.0));

    // A repeat of the same count is not a sample: it must not drag the average down
    // while the model is thinking.
    const double before = tokensPerSecond(c, 2400);
    noteTokens(c, 2400, 200);
    CHECK(tokensPerSecond(c, 2400) == doctest::Approx(before));

    // Under 300 ms of span the figure is noise, so it reads 0.
    Clock fast;
    begin(fast, 0, 0);
    noteTokens(fast, 100, 40);
    CHECK(tokensPerSecond(fast, 100) == doctest::Approx(0.0));

    // Once the window has gone quiet (no emission for two seconds) the rate falls
    // to 0 instead of freezing at its last value.
    CHECK(tokensPerSecond(c, 6000) == doctest::Approx(0.0));

    // A stopped clock records nothing.
    end(c);
    noteTokens(c, 6200, 999);
    CHECK(tokensPerSecond(c, 6200) == doctest::Approx(0.0));
}

// ---- the machine sampler's unit rule ------------------------------------

TEST_CASE("system stats: CPU percent uses the whole machine, like Task Manager") {
    // One second of CPU burned over one second of wall clock on four cores is a
    // quarter of the machine -- the number a task manager shows.
    CHECK(cpuPercentBetween(1.0, 2.0, 1.0, 4) == doctest::Approx(25.0));
    CHECK(cpuPercentBetween(0.0, 1.0, 1.0, 1) == doctest::Approx(100.0));
    // A shorter window with the same CPU time is a higher percentage.
    CHECK(cpuPercentBetween(0.0, 1.0, 0.5, 4) == doctest::Approx(50.0));
    // A child that exited lowers the tree's cumulative total: the rate must clamp at
    // zero rather than report a negative percentage.
    CHECK(cpuPercentBetween(5.0, 4.0, 1.0, 4) == doctest::Approx(0.0));
    // A zero-length window (two samples in the same millisecond) has no rate.
    CHECK(cpuPercentBetween(0.0, 1.0, 0.0, 4) == doctest::Approx(0.0));
    // Never above 100: a busy process cannot be busier than the machine.
    CHECK(cpuPercentBetween(0.0, 10.0, 1.0, 1) == doctest::Approx(100.0));
    // A nonsense core count is treated as one core, not as a division by zero.
    CHECK(cpuPercentBetween(0.0, 1.0, 1.0, 0) == doctest::Approx(100.0));
}
