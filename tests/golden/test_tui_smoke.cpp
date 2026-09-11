// FTXUI integration smoke test: render DOM to an in-memory Screen (no TTY needed)
// and assert text, including CJK wide characters, survives the render pipeline.
#include "doctest.h"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include <string>

using namespace ftxui;

TEST_CASE("FTXUI renders text to screen (smoke)") {
    auto document = vbox({
        text("proJV TUI smoke"),
        separator(),
        text("中文测试 Chinese"),
    });

    auto screen = Screen::Create(Dimension::Fixed(60), Dimension::Fixed(8));
    Render(screen, document);

    std::string out = screen.ToString();
    CHECK_FALSE(out.empty());
    CHECK(out.find("proJV TUI smoke") != std::string::npos);
    // CJK must survive the UTF-8 render path.
    CHECK(out.find("中文测试 Chinese") != std::string::npos);
}
