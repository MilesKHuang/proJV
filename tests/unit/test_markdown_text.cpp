// Markdown parsing unit tests -- lock the 1:1 rules from legacy markdown_render.
#include "doctest.h"

#include "tui/markdown_text.h"

#include <string>

using namespace markdown_text;

TEST_CASE("markdown: inline styles (bold/italic/code/link)") {
    auto lines = parseMarkdown("hello **bold** and *italic* and `code` and [link](http://x)");
    REQUIRE(lines.size() == 1);

    bool hasBold = false, hasItalic = false, hasCode = false, hasLink = false;
    for (const auto& s : lines[0].segs) {
        if (s.style == Style::Bold && s.text == "bold") hasBold = true;
        if (s.style == Style::Italic && s.text == "italic") hasItalic = true;
        if (s.style == Style::Code && s.text == "code") hasCode = true;
        if (s.style == Style::Link && s.text == "link" && s.url == "http://x") hasLink = true;
    }
    CHECK(hasBold);
    CHECK(hasItalic);
    CHECK(hasCode);
    CHECK(hasLink);
}

TEST_CASE("markdown: headings H1/H2/H3") {
    auto lines = parseMarkdown("# One\n## Two\n### Three\n#### Four");
    REQUIRE(lines.size() == 4);
    CHECK(lines[0].segs[0].style == Style::H1);
    CHECK(lines[0].segs[0].text == "One");
    CHECK(lines[1].segs[0].style == Style::H2);
    CHECK(lines[1].segs[0].text == "Two");
    CHECK(lines[2].segs[0].style == Style::H3);
    CHECK(lines[2].segs[0].text == "Three");
    // Level >=3 collapses to H3.
    CHECK(lines[3].segs[0].style == Style::H3);
    CHECK(lines[3].segs[0].text == "Four");
}

TEST_CASE("markdown: blockquote") {
    auto lines = parseMarkdown("> quoted text");
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].segs[0].style == Style::Quote);
    CHECK(lines[0].segs[0].text == "quoted text");
}

TEST_CASE("markdown: unordered and ordered lists") {
    auto lines = parseMarkdown("- item one\n* item two\n1. first\n2. second");
    REQUIRE(lines.size() == 4);

    CHECK(lines[0].segs[0].style == Style::Bullet);
    CHECK(lines[0].segs[0].text == "item one");

    CHECK(lines[1].segs[0].style == Style::Bullet);
    CHECK(lines[1].segs[0].text == "item two");

    CHECK(lines[2].segs[0].style == Style::Ordered);
    CHECK(lines[2].segs[0].text == "1.");
    CHECK(lines[2].segs[1].style == Style::Ordered);
    CHECK(lines[2].segs[1].text == "first");

    CHECK(lines[3].segs[0].text == "2.");
    CHECK(lines[3].segs[1].text == "second");
}

TEST_CASE("markdown: horizontal rule") {
    auto lines = parseMarkdown("before\n---\nafter");
    REQUIRE(lines.size() == 3);
    CHECK(lines[1].segs[0].style == Style::HR);
}

TEST_CASE("markdown: code block") {
    auto lines = parseMarkdown("```\nint x = 1;\nreturn x;\n```");
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].segs[0].style == Style::CodeBlock);
    CHECK(lines[0].segs[0].text == "int x = 1;");
    CHECK(lines[1].segs[0].style == Style::CodeBlock);
    CHECK(lines[1].segs[0].text == "return x;");
}

TEST_CASE("markdown: table header and rows") {
    auto lines = parseMarkdown("| A | B |\n|---|---|\n| 1 | 2 |");
    REQUIRE(lines.size() == 2);

    // Header row.
    REQUIRE(lines[0].segs.size() == 2);
    CHECK(lines[0].segs[0].style == Style::TableHeader);
    CHECK(lines[0].segs[0].text == "A");
    CHECK(lines[0].segs[1].style == Style::TableHeader);
    CHECK(lines[0].segs[1].text == "B");

    // Data row.
    REQUIRE(lines[1].segs.size() == 2);
    CHECK(lines[1].segs[0].style == Style::TableCell);
    CHECK(lines[1].segs[0].text == "1");
    CHECK(lines[1].segs[1].style == Style::TableCell);
    CHECK(lines[1].segs[1].text == "2");
}

TEST_CASE("markdown: table without leading pipe (DeepSeek style)") {
    auto lines = parseMarkdown("Name | Value\n---|---\nalpha | 1\nbeta | 2");
    REQUIRE(lines.size() == 3);

    // Header row: the segment before the first '|' must be kept.
    REQUIRE(lines[0].segs.size() == 2);
    CHECK(lines[0].segs[0].style == Style::TableHeader);
    CHECK(lines[0].segs[0].text == "Name");
    CHECK(lines[0].segs[1].text == "Value");

    // Data rows keep their first column too.
    REQUIRE(lines[1].segs.size() == 2);
    CHECK(lines[1].segs[0].style == Style::TableCell);
    CHECK(lines[1].segs[0].text == "alpha");
    CHECK(lines[1].segs[1].text == "1");

    REQUIRE(lines[2].segs.size() == 2);
    CHECK(lines[2].segs[0].text == "beta");
    CHECK(lines[2].segs[1].text == "2");
}

TEST_CASE("markdown: table without leading pipe on separator row") {
    auto lines = parseMarkdown("A | B\n--- | ---\n1 | 2");
    REQUIRE(lines.size() == 2);

    REQUIRE(lines[0].segs.size() == 2);
    CHECK(lines[0].segs[0].text == "A");
    CHECK(lines[0].segs[1].text == "B");

    REQUIRE(lines[1].segs.size() == 2);
    CHECK(lines[1].segs[0].text == "1");
    CHECK(lines[1].segs[1].text == "2");
}

TEST_CASE("markdown: table column alignment") {
    auto lines = parseMarkdown("| A | B | C |\n|:---|---:|:---:|\n| 1 | 2 | 3 |");
    REQUIRE(lines.size() == 2);
    REQUIRE(lines[0].tableAlign.size() == 3);
    CHECK(lines[0].tableAlign[0] == 0);  // left
    CHECK(lines[0].tableAlign[1] == 2);  // right
    CHECK(lines[0].tableAlign[2] == 1);  // center
}

TEST_CASE("markdown: table followed by paragraph") {
    auto lines = parseMarkdown("| A | B |\n|---|---|\n| 1 | 2 |\nnext paragraph");
    REQUIRE(lines.size() == 3);
    CHECK(lines[0].segs[0].style == Style::TableHeader);
    CHECK(lines[1].segs[0].style == Style::TableCell);
    CHECK(lines[2].segs[0].style == Style::Normal);
    CHECK(lines[2].segs[0].text == "next paragraph");
}

TEST_CASE("markdown: paragraph followed by table") {
    auto lines = parseMarkdown("intro text\n| A | B |\n|---|---|\n| 1 | 2 |");
    REQUIRE(lines.size() == 3);
    CHECK(lines[0].segs[0].style == Style::Normal);
    CHECK(lines[0].segs[0].text == "intro text");
    CHECK(lines[1].segs[0].style == Style::TableHeader);
    CHECK(lines[2].segs[0].style == Style::TableCell);
}

TEST_CASE("markdown: two tables separated by blank line") {
    auto lines = parseMarkdown("| A |\n|---|\n| 1 |\n\n| B |\n|---|\n| 2 |");
    REQUIRE(lines.size() == 5);
    CHECK(lines[0].segs[0].style == Style::TableHeader);
    CHECK(lines[0].segs[0].text == "A");
    CHECK(lines[1].segs[0].style == Style::TableCell);
    CHECK(lines[1].segs[0].text == "1");
    CHECK(lines[2].segs.empty());  // blank line between tables
    CHECK(lines[3].segs[0].style == Style::TableHeader);
    CHECK(lines[3].segs[0].text == "B");
    CHECK(lines[4].segs[0].style == Style::TableCell);
    CHECK(lines[4].segs[0].text == "2");
}
