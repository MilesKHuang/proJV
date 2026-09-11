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
