// proJV -- exec_shell output shaping (src/tools/shell_output.h).
//
// The rules under test are the ones a regression would silently break:
//   * the TAIL survives (the verdict lives there);
//   * short output is byte-identical (no accidental reformatting);
//   * the cut never lands inside a UTF-8 character;
//   * the notice states how much was dropped, so the model can ask for more.
#include "doctest/doctest.h"
#include "tools/shell_output.h"

#include <string>

namespace {

std::string numberedLines(size_t count) {
    std::string s;
    for (size_t i = 1; i <= count; ++i) {
        s += "line " + std::to_string(i) + "\n";
    }
    return s;
}

bool startsInsideUtf8Character(const std::string& s) {
    return !s.empty() && (static_cast<unsigned char>(s[0]) & 0xC0) == 0x80;
}

} // namespace

TEST_CASE("shell_output: short output is returned byte-identical") {
    const std::string in = "hello\nworld\n";
    const auto a = shell_output::apply(in, 400, false);
    CHECK(a.text == in);
    CHECK_FALSE(a.truncated);
    CHECK(a.droppedLines == 0);
    CHECK(a.totalBytes == in.size());
}

TEST_CASE("shell_output: empty output stays empty") {
    const auto a = shell_output::apply("", 400, false);
    CHECK(a.text.empty());
    CHECK_FALSE(a.truncated);
}

TEST_CASE("shell_output: the tail survives and the notice counts dropped lines") {
    const std::string in = numberedLines(1000);
    const auto a = shell_output::apply(in, 400, false);
    CHECK(a.truncated);
    CHECK(a.droppedLines == 600);
    CHECK(a.text.find("600 earlier line(s) omitted") != std::string::npos);
    CHECK(a.text.find("line 1000") != std::string::npos);  // the verdict line
    CHECK(a.text.find("line 601") != std::string::npos);   // first kept line
    CHECK(a.text.find("line 600\n") == std::string::npos); // first dropped line
    CHECK(a.text.find("line 1\n") == std::string::npos);
}

TEST_CASE("shell_output: exactly tail_lines lines is not truncated") {
    const std::string in = numberedLines(400);
    const auto a = shell_output::apply(in, 400, false);
    CHECK_FALSE(a.truncated);
    CHECK(a.text.find("line 1\n") != std::string::npos);
}

TEST_CASE("shell_output: tail_lines is clamped to [10, 2000]") {
    const std::string in = numberedLines(50);
    // 1 clamps up to 10 -> 40 dropped.
    {
        const auto a = shell_output::apply(in, 1, false);
        CHECK(a.truncated);
        CHECK(a.droppedLines == 40);
        CHECK(a.text.find("line 41") != std::string::npos); // first kept line
        CHECK(a.text.find("line 40\n") == std::string::npos);
    }
    // 0 means "use the default".
    {
        const auto a = shell_output::apply(in, 0, false);
        CHECK_FALSE(a.truncated);
    }
    // 99999 clamps down to 2000: 3000 lines -> 1000 dropped.
    {
        const std::string big = numberedLines(3000);
        const auto a = shell_output::apply(big, 99999, false);
        CHECK(a.droppedLines == 1000);
    }
}

TEST_CASE("shell_output: deep tail_lines keeps more than the default") {
    const std::string in = numberedLines(1000);
    const auto a = shell_output::apply(in, 2000, false);
    CHECK_FALSE(a.truncated);
    CHECK(a.text == in);
}

TEST_CASE("shell_output: full_output under the ceiling is untouched") {
    const std::string in = numberedLines(5000); // ~50 KB
    const auto a = shell_output::apply(in, 400, true);
    CHECK_FALSE(a.truncated);
    CHECK(a.text == in);
}

TEST_CASE("shell_output: full_output over the ceiling keeps the last 200 KB") {
    std::string in;
    in.reserve(300u * 1024u);
    while (in.size() < 300u * 1024u)
        in += "abcdefghij0123456789\n";
    in += "THE-END\n";
    const auto a = shell_output::apply(in, 400, true);
    CHECK(a.truncated);
    CHECK(a.text.find("output truncated: kept 200.0 KB") != std::string::npos);
    CHECK(a.text.find("THE-END") != std::string::npos); // tail kept
    CHECK(a.text.find("grep -i error") != std::string::npos);
    CHECK(a.text.size() < shell_output::kFullOutputMaxBytes + 512);
}

TEST_CASE("shell_output: the cut never starts inside a UTF-8 character") {
    // 3-byte characters, so a naive byte cut lands mid-character 2 times in 3.
    std::string in;
    for (int i = 0; i < 3000; ++i)
        in += "\xe4\xb8\xad"; // U+4E2D
    for (int i = 0; i < 3000; ++i) {
        in += "\n";
        in += "\xe4\xb8\xad";
    }
    std::string s = in;
    const bool cut = shell_output::keepTailBytes(s, 4001);
    CHECK(cut);
    REQUIRE_FALSE(s.empty());
    CHECK_FALSE(startsInsideUtf8Character(s));
    // The newline bytes shift the alignment, so only the bounds are fixed.
    CHECK(s.size() <= 4001);
    CHECK(s.size() >= 3999);
}

TEST_CASE("shell_output: a multi-byte tail is not cut mid-character either") {
    std::string in;
    for (int i = 0; i < 2000; ++i)
        in += "\xe4\xb8\xad line " + std::to_string(i) + "\n";
    const auto a = shell_output::apply(in, 10, false);
    CHECK(a.truncated);
    // The notice prefix is ASCII; everything after it must start on a boundary.
    const size_t body = a.text.find('\n');
    REQUIRE(body != std::string::npos);
    CHECK_FALSE(startsInsideUtf8Character(a.text.substr(body + 1)));
}

TEST_CASE("shell_output: runner guard keeps head, notice and tail") {
    std::string in(6u * 1024u * 1024u, 'x');
    in.replace(0, 11, "HEAD-MARK  ");
    in.replace(in.size() - 9, 9, "TAIL-MARK");
    shell_output::keepTailWithMiddleNotice(in, shell_output::kRunnerKeepBytes);
    CHECK(in.find("HEAD-MARK") != std::string::npos);
    CHECK(in.find("TAIL-MARK") != std::string::npos);
    CHECK(in.find("dropped from the middle") != std::string::npos);
    CHECK(in.size() < shell_output::kRunnerKeepBytes + 512);
}

TEST_CASE("shell_output: runner guard leaves a small output alone") {
    const std::string in = "small";
    std::string s = in;
    shell_output::keepTailWithMiddleNotice(s, shell_output::kRunnerKeepBytes);
    CHECK(s == in);
}
