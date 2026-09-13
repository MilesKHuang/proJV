// ThemeColors model tests -- verify the 74-field list is complete and JSON round-trips.
#include "doctest.h"

#include "tui/theme_colors.h"

#include <string>

TEST_CASE("theme_colors: field list is complete (74 fields)") {
    // The legacy ThemeColors had 74 color fields. If this changes, the migration
    // dropped or added a field -- investigate rather than silently updating.
    CHECK(themeColorFields().size() == 74);
}

TEST_CASE("theme_colors: JSON round-trip preserves all fields") {
    auto obs = ThemeColors::obsidian();
    std::string json = obs.toJson();
    auto parsed = ThemeColors::fromJson(json);

    CHECK(parsed.name == obs.name);
    CHECK(parsed.author == obs.author);
    for (const auto& f : themeColorFields()) {
        INFO(f.name);
        CHECK(parsed.*f.member == obs.*f.member);
    }
}

TEST_CASE("theme_colors: light theme name") {
    CHECK(ThemeColors::light().name == "Light");
}

TEST_CASE("theme_colors: fromJson falls back for missing fields") {
    ThemeColors tc = ThemeColors::fromJson(R"({"name":"X","colors":{}})");
    CHECK(tc.name == "X");
    // Missing fields keep the Obsidian default.
    CHECK(tc.windowBg == ThemeColors::obsidian().windowBg);
}
