// diff_capture: the unified diff a file-writing tool reports to the TUI.
//
// The diff is display-only (its own bubble + a column in the session database),
// so these cases lock three things: it is readable for an ordinary edit, it does
// not invent a whole-file change out of a line-ending mismatch, and it always
// terminates on large or binary input.
#include "doctest.h"

#include "tools/diff_capture.h"

#include <string>

using namespace diff_capture;

namespace {

std::string numberedLines(int count, const std::string& prefix) {
    std::string out;
    for (int i = 0; i < count; ++i) {
        out += prefix;
        out += std::to_string(i);
        out += '\n';
    }
    return out;
}

} // namespace

TEST_CASE("diff_capture: one changed line gives one - and one + with context") {
    const std::string diff =
        makeUnifiedDiff("alpha\nbeta\ngamma\n", "alpha\nBETA\ngamma\n", "src/a.cpp");

    CHECK(diff.find("--- a/src/a.cpp") != std::string::npos);
    CHECK(diff.find("+++ b/src/a.cpp") != std::string::npos);
    CHECK(diff.find("@@ -1,3 +1,3 @@") != std::string::npos);
    CHECK(diff.find("\n-beta\n") != std::string::npos);
    CHECK(diff.find("\n+BETA\n") != std::string::npos);
    CHECK(diff.find("\n alpha\n") != std::string::npos);
    CHECK(diff.find("\n gamma\n") != std::string::npos);

    // Exactly one removal and one addition; the "--- "/"+++ " headers do not
    // count as patch lines.
    int removals = 0;
    int additions = 0;
    size_t pos = 0;
    while (pos < diff.size()) {
        const size_t nl = diff.find('\n', pos);
        if (nl == std::string::npos) break;
        const std::string line = diff.substr(pos, nl - pos);
        if (line.size() > 1 && line[0] == '-' && line.compare(0, 4, "--- ") != 0) ++removals;
        if (line.size() > 1 && line[0] == '+' && line.compare(0, 4, "+++ ") != 0) ++additions;
        pos = nl + 1;
    }
    CHECK(removals == 1);
    CHECK(additions == 1);
}

TEST_CASE("diff_capture: a new file is all additions and starts at -0,0") {
    const std::string diff = makeUnifiedDiff("", "hello\nworld\n", "a.txt");
    CHECK(diff.find("--- a/a.txt") != std::string::npos);
    CHECK(diff.find("@@ -0,0 +1,2 @@") != std::string::npos);
    CHECK(diff.find("\n+hello\n") != std::string::npos);
    CHECK(diff.find("\n+world\n") != std::string::npos);
    CHECK(diff.find("\n-") == std::string::npos);
}

TEST_CASE("diff_capture: identical text produces no diff at all") {
    CHECK(makeUnifiedDiff("same\n", "same\n", "a.txt").empty());
    CHECK(makeUnifiedDiff("", "", "a.txt").empty());
    // A pure line-ending flip is not a content change either.
    CHECK(makeUnifiedDiff("a\r\nb\r\n", "a\nb\n", "a.txt").empty());
}

TEST_CASE("diff_capture: CRLF on disk does not read as a whole-file change") {
    // The writing tools snapshot a CRLF file and record LF model text; without
    // the CR strip every line would look changed.
    const std::string diff = makeUnifiedDiff("a\r\nb\r\nc\r\n", "a\r\nB\r\nc\r\n", "a.txt");
    CHECK(diff.find("\n-b\n") != std::string::npos);
    CHECK(diff.find("\n+B\n") != std::string::npos);
    CHECK(diff.find("\r") == std::string::npos);
    CHECK(diff.find("@@ -1,3 +1,3 @@") != std::string::npos);
}

TEST_CASE("diff_capture: binary content is never rendered") {
    std::string before = "abc";
    before.push_back('\0');
    before += "def";
    CHECK(makeUnifiedDiff(before, "abc\ndef\n", "a.bin").empty());
    CHECK(makeUnifiedDiff("abc\n", before, "a.bin").empty());
}

TEST_CASE("diff_capture: an oversized patch is truncated with a marker") {
    const std::string before = numberedLines(300, "old");
    const std::string after = numberedLines(300, "new");
    const std::string diff = makeUnifiedDiff(before, after, "big.txt");

    CHECK(!diff.empty());
    CHECK(diff.find("diff truncated") != std::string::npos);
    CHECK(diff.find("more lines)") != std::string::npos);
    // maxLines default is 200, plus the truncation marker.
    int lines = 0;
    for (char c : diff)
        if (c == '\n') ++lines;
    CHECK(lines == 201);
}

TEST_CASE("diff_capture: the character cap also truncates") {
    // One very long changed line, then enough other changes to overflow maxChars.
    const std::string before = "x\n" + numberedLines(40, "a");
    const std::string after = "y\n" + numberedLines(40, "b");
    const std::string diff = makeUnifiedDiff(before, after, "big.txt", 3, 200, 120);
    CHECK(diff.size() < 400); // far below the untruncated size
    CHECK(diff.find("diff truncated") != std::string::npos);
}

TEST_CASE("diff_capture: take hands the entry over exactly once") {
    reset();
    record("f.txt", "a\n", "b\n");

    const Entry first = take();
    CHECK(first.path == "f.txt");
    CHECK(first.unified.find("\n-a\n") != std::string::npos);
    CHECK(first.unified.find("\n+b\n") != std::string::npos);

    // Taken, so the next call sees nothing.
    CHECK(take().unified.empty());
    CHECK(take().path.empty());
}

TEST_CASE("diff_capture: reset drops a pending entry") {
    reset();
    record("f.txt", "a\n", "b\n");
    reset();
    CHECK(take().unified.empty());
}

TEST_CASE("diff_capture: recording a no-op change creates no entry") {
    reset();
    record("f.txt", "same\n", "same\n");
    CHECK(take().unified.empty());
}

TEST_CASE("diff_capture: displayPath shortens and normalises") {
    // No workspace: the path is only normalised, never shortened.
    CHECK(displayPath("src\\tools\\x.cpp", "") == "src/tools/x.cpp");
    CHECK(displayPath("a.txt", "") == "a.txt");
}
