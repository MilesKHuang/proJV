// speaker_colors unit tests -- lock the deterministic, theme-driven mapping.
#include "doctest.h"

#include "tui/speaker_colors.h"

#include <set>
#include <string>

TEST_CASE("speaker_colors: palette is non-trivial") {
    CHECK(speaker_colors::paletteCount() >= 4);
}

TEST_CASE("speaker_colors: slot is deterministic per name") {
    CHECK(speaker_colors::paletteSlot("Sheldon") == speaker_colors::paletteSlot("Sheldon"));
    CHECK(speaker_colors::paletteSlot("DeadCode Auditor")
          == speaker_colors::paletteSlot("DeadCode Auditor"));
}

TEST_CASE("speaker_colors: slot is always in range") {
    for (const std::string& n : {"", "Supervisor", "Formatter", "Translator",
                                 "DeadCode Auditor", "Sheldon", "Penny", "Leonard"}) {
        CHECK(speaker_colors::paletteSlot(n) < speaker_colors::paletteCount());
    }
}

TEST_CASE("speaker_colors: a role set spreads across more than one slot") {
    // Not a strict guarantee per name, but a realistic skill's roles should not
    // all collapse onto one color (which would defeat the whole point).
    std::set<std::size_t> slots;
    for (const std::string& n : {"Supervisor", "Formatter", "Translator", "DeadCode Auditor"})
        slots.insert(speaker_colors::paletteSlot(n));
    CHECK(slots.size() >= 2);
}
