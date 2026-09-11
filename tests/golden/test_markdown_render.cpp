// Markdown render golden test (table rendering check).
#include "doctest.h"

#include "tui/markdown_view.h"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include <string>

using namespace ftxui;

TEST_CASE("markdown render: table") {
    auto doc = markdown_view::renderMarkdown("| A | B |\n|---|---|\n| 1 | 2 |");
    auto screen = Screen::Create(Dimension::Fixed(40), Dimension::Fixed(6));
    Render(screen, doc);
    std::string out = screen.ToString();
    CHECK(out.find("A") != std::string::npos);
    CHECK(out.find("B") != std::string::npos);
    CHECK(out.find("1") != std::string::npos);
    CHECK(out.find("2") != std::string::npos);
}

TEST_CASE("markdown render: table draws box-drawing border") {
    auto doc = markdown_view::renderMarkdown("| A | B |\n|---|---|\n| 1 | 2 |");
    auto screen = Screen::Create(Dimension::Fixed(40), Dimension::Fixed(8));
    Render(screen, doc);
    std::string out = screen.ToString();

    CHECK(out.find("A") != std::string::npos);
    CHECK(out.find("B") != std::string::npos);
    CHECK(out.find("1") != std::string::npos);
    CHECK(out.find("2") != std::string::npos);

    // ftxui::Table renders box-drawing borders (DOUBLE: ═ and ║).
    CHECK(out.find("\u2550") != std::string::npos);  // ═
    CHECK(out.find("\u2551") != std::string::npos);  // ║
}

TEST_CASE("markdown render: table columns have separators") {
    auto doc = markdown_view::renderMarkdown("| Name | Value |\n|---|---|\n| a | b |");
    auto screen = Screen::Create(Dimension::Fixed(30), Dimension::Fixed(6));
    Render(screen, doc);
    std::string out = screen.ToString();

    size_t bars = 0;
    size_t pos = 0;
    while ((pos = out.find("\u2551", pos)) != std::string::npos) { ++bars; pos += 3; }
    // Left border + column separator + right border = at least 3 vertical bars.
    CHECK(bars >= 3);
}

TEST_CASE("markdown render: blockquote keeps legacy left bar") {
    auto doc = markdown_view::renderMarkdown("> quoted text");
    auto screen = Screen::Create(Dimension::Fixed(30), Dimension::Fixed(4));
    Render(screen, doc);
    std::string out = screen.ToString();
    CHECK(out.find("quoted text") != std::string::npos);
    // Legacy drew an mdQuoteBar strip; TUI renders a │ prefix.
    CHECK(out.find("\u2502") != std::string::npos);
}

TEST_CASE("markdown render: code block renders as one block") {
    auto doc = markdown_view::renderMarkdown("```\nint x = 1;\nreturn x;\n```");
    auto screen = Screen::Create(Dimension::Fixed(40), Dimension::Fixed(8));
    Render(screen, doc);
    std::string out = screen.ToString();
    CHECK(out.find("int x = 1;") != std::string::npos);
    CHECK(out.find("return x;") != std::string::npos);
}

TEST_CASE("markdown render: CJK text soft-wraps without truncation") {
    std::string text = "一二三四五六七八九十甲乙丙丁戊己庚辛";  // 20 CJK chars
    auto doc = markdown_view::renderMarkdown(text);
    auto screen = Screen::Create(Dimension::Fixed(12), Dimension::Fixed(10));
    Render(screen, doc);
    std::string out = screen.ToString();

    // First and last characters must both survive (nothing clipped).
    CHECK(out.find("一") != std::string::npos);
    CHECK(out.find("辛") != std::string::npos);

    // They must land on different lines (wrapped, not clipped to one line).
    std::vector<std::string> rows;
    std::string cur;
    for (char c : out) {
        if (c == '\n') { rows.push_back(cur); cur.clear(); }
        else cur += c;
    }
    rows.push_back(cur);
    int firstRow = -1, lastRow = -1;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].find("一") != std::string::npos) firstRow = static_cast<int>(i);
        if (rows[i].find("辛") != std::string::npos) lastRow = static_cast<int>(i);
    }
    CHECK(firstRow != -1);
    CHECK(lastRow != -1);
    CHECK(firstRow != lastRow);
}

TEST_CASE("markdown render: long unbroken word wraps character-by-character") {
    std::string text = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMN";  // 40 chars, no spaces
    auto doc = markdown_view::renderMarkdown(text);
    auto screen = Screen::Create(Dimension::Fixed(20), Dimension::Fixed(6));
    Render(screen, doc);
    std::string out = screen.ToString();

    CHECK(out.find("a") != std::string::npos);
    CHECK(out.find("N") != std::string::npos);
}

TEST_CASE("markdown render: long paragraph soft-wraps without truncation") {
    std::string text = "alpha beta gamma delta epsilon zeta eta theta";
    auto doc = markdown_view::renderMarkdown(text);
    auto screen = Screen::Create(Dimension::Fixed(20), Dimension::Fixed(8));
    Render(screen, doc);
    std::string out = screen.ToString();

    // Every word must survive the wrap.
    CHECK(out.find("alpha") != std::string::npos);
    CHECK(out.find("theta") != std::string::npos);

    // The first and last words must land on different lines (wrapped).
    std::vector<std::string> rows;
    std::string cur;
    for (char c : out) {
        if (c == '\n') { rows.push_back(cur); cur.clear(); }
        else cur += c;
    }
    rows.push_back(cur);

    int alphaRow = -1, thetaRow = -1;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].find("alpha") != std::string::npos) alphaRow = static_cast<int>(i);
        if (rows[i].find("theta") != std::string::npos) thetaRow = static_cast<int>(i);
    }
    CHECK(alphaRow != -1);
    CHECK(thetaRow != -1);
    CHECK(alphaRow != thetaRow);
}
