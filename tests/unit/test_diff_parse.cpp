// diff_view: turning the stored unified diff into a two-column layout.
//
// The parse is the risky half of the side-by-side view: line numbers must keep
// counting across hunks, a run of removals must pair with the run of additions,
// and nothing may be dropped when a line is not patch syntax.
#include "doctest.h"

#include "tui/diff_view.h"

#include <string>
#include <vector>

using namespace diff_view;

namespace {

const char* kOneChange = "--- a/x.cpp\n"
                         "+++ b/x.cpp\n"
                         "@@ -10,7 +10,7 @@\n"
                         " line10\n"
                         " line11\n"
                         " line12\n"
                         "-old13\n"
                         "+new13\n"
                         " line14\n"
                         " line15\n"
                         " line16\n";

} // namespace

TEST_CASE("diff_view: file headers are dropped and hunk headers kept") {
    const std::vector<Row> rows = alignRows(kOneChange);
    REQUIRE_FALSE(rows.empty());
    CHECK(rows[0].isHeader);
    CHECK(rows[0].headerText == "@@ -10,7 +10,7 @@");
    for (const auto& r : rows) {
        CHECK(r.headerText.find("--- a/") == std::string::npos);
        CHECK(r.headerText.find("+++ b/") == std::string::npos);
    }
}

TEST_CASE("diff_view: line numbers advance on both sides and skip per side") {
    const std::vector<Row> rows = alignRows(kOneChange);
    // header + line11 + line12 + the change + line14 + line15 (outer context cut)
    REQUIRE(rows.size() == 6);

    CHECK(rows[1].oldNo == 11);
    CHECK(rows[1].newNo == 11);
    CHECK(rows[1].oldText == "line11");
    CHECK_FALSE(rows[1].oldIsChange);
    CHECK_FALSE(rows[1].newIsChange);

    // The change keeps both numbers on one row, like VSCode.
    CHECK(rows[3].oldNo == 13);
    CHECK(rows[3].newNo == 13);
    CHECK(rows[3].oldText == "old13");
    CHECK(rows[3].newText == "new13");
    CHECK(rows[3].oldIsChange);
    CHECK(rows[3].newIsChange);

    CHECK(rows[5].oldNo == 15);
    CHECK(rows[5].newNo == 15);
}

TEST_CASE("diff_view: context is trimmed to the requested depth") {
    const std::vector<Row> twoEach = alignRows(kOneChange, 2);
    CHECK(twoEach.size() == 6); // header + 2 + change + 2

    const std::vector<Row> none = alignRows(kOneChange, 0);
    REQUIRE(none.size() == 2); // header + the change
    CHECK(none[1].oldIsChange);
    CHECK(none[1].oldNo == 13);

    const std::vector<Row> all = alignRows(kOneChange, 99);
    CHECK(all.size() == 8); // header + 3 + change + 3
}

TEST_CASE("diff_view: three removals and one addition pair by index") {
    const std::string patch = "@@ -1,1 +1,1 @@\n"
                              "-a\n"
                              "-b\n"
                              "-c\n"
                              "+d\n";
    const std::vector<Row> rows = alignRows(patch, 2);
    REQUIRE(rows.size() == 4); // header + 3 paired rows
    CHECK(rows[1].oldNo == 1);
    CHECK(rows[1].newNo == 1);
    CHECK(rows[1].oldText == "a");
    CHECK(rows[1].newText == "d");
    // Leftovers exist on the removed side only.
    CHECK(rows[2].oldNo == 2);
    CHECK(rows[2].newNo == 0);
    CHECK(rows[2].newText.empty());
    CHECK(rows[3].oldNo == 3);
    CHECK(rows[3].newNo == 0);
}

TEST_CASE("diff_view: a new file starts at line 1 on the added side only") {
    const std::string patch = "@@ -0,0 +1,2 @@\n"
                              "+hello\n"
                              "+world\n";
    const std::vector<Row> rows = alignRows(patch, 2);
    REQUIRE(rows.size() == 3);
    CHECK(rows[1].oldNo == 0);
    CHECK(rows[1].newNo == 1);
    CHECK(rows[1].newIsChange);
    CHECK_FALSE(rows[1].oldIsChange);
    CHECK(rows[1].newText == "hello");
    CHECK(rows[2].newNo == 2);
}

TEST_CASE("diff_view: the truncation marker survives as a full-width row") {
    const std::string patch = "@@ -1,1 +1,1 @@\n"
                              "-a\n"
                              "+b\n"
                              "... diff truncated (12 more lines)\n";
    const std::vector<Row> rows = alignRows(patch, 2);
    REQUIRE(rows.size() == 3);
    CHECK(rows[2].isHeader);
    CHECK(rows[2].headerText.find("diff truncated") != std::string::npos);
}

TEST_CASE("diff_view: empty and non-patch input never crashes or vanishes") {
    CHECK(alignRows("").empty());
    CHECK(alignRows("not a diff at all\n").size() == 1);

    // A header the parser cannot read keeps the body visible with blank gutters.
    const std::string odd = "@@ nonsense @@\n body\n";
    const std::vector<Row> rows = alignRows(odd, 2);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].isHeader);
    CHECK(rows[1].oldNo == 0);
    CHECK(rows[1].newNo == 0);
    CHECK(rows[1].oldText == "body");
}

TEST_CASE("diff_view: the row cap keeps the head and reports the total") {
    std::string patch = "@@ -1,40 +1,40 @@\n";
    for (int i = 0; i < 40; ++i) {
        patch += "-old" + std::to_string(i) + "\n";
        patch += "+new" + std::to_string(i) + "\n";
    }
    int total = 0;
    const std::vector<Row> rows = alignRows(patch, 2, 5, &total);
    CHECK(total == 41); // header + 40 paired rows
    CHECK(rows.size() == 5);
    CHECK(rows[1].oldText == "old0");
}

TEST_CASE("diff_view: numbers restart from each hunk header") {
    const std::string patch = "--- a/x\n"
                              "+++ b/x\n"
                              "@@ -1,1 +1,1 @@\n"
                              "-a\n"
                              "+b\n"
                              "@@ -100,1 +200,1 @@\n"
                              "-c\n"
                              "+d\n";
    const std::vector<Row> rows = alignRows(patch, 2);
    REQUIRE(rows.size() == 4);
    CHECK(rows[1].oldNo == 1);
    CHECK(rows[1].newNo == 1);
    CHECK(rows[3].oldNo == 100);
    CHECK(rows[3].newNo == 200);
}
