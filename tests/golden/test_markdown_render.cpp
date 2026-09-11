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

    // ftxui::Table renders box-drawing borders (UTF-8: ─ and │).
    CHECK(out.find("\u2500") != std::string::npos);  // ─
    CHECK(out.find("\u2502") != std::string::npos);  // │
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
