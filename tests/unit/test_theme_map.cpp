// theme_map tests.
#include "doctest.h"

#include "tui/theme_map.h"

#include <string>

using ftxui::Color;

TEST_CASE("theme_map: hex to RGB") {
    CHECK(theme_map::hexToColor("#FF0000") == Color::RGB(255, 0, 0));
    CHECK(theme_map::hexToColor("#00FF00") == Color::RGB(0, 255, 0));
    CHECK(theme_map::hexToColor("#0000FF") == Color::RGB(0, 0, 255));
}

TEST_CASE("theme_map: mixed hex") {
    CHECK(theme_map::hexToColor("#1E2840") == Color::RGB(30, 40, 64));
}

TEST_CASE("theme_map: malformed returns white") {
    CHECK(theme_map::hexToColor("not-a-color") == Color::RGB(255, 255, 255));
}
