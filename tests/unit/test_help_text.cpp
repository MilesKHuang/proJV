// proJV -- the single command table (src/core/help_text.h).
//
// The point of these assertions is drift, not formatting: /help, the "/" menu and
// the F1 panel all read one table, and the menu's own clipping must not leak into
// the other two readers.
#include "doctest/doctest.h"

#include "core/help_text.h"
#include "tui/slash_menu.h"

#include <algorithm>
#include <set>
#include <string>

namespace {

bool hasName(const std::vector<help_text::CmdHint>& v, const std::string& name) {
    return std::any_of(v.begin(), v.end(),
                       [&](const help_text::CmdHint& c) { return c.name == name; });
}

} // namespace

TEST_CASE("help_text: every implemented command is listed") {
    const auto& cmds = help_text::commands();
    REQUIRE(!cmds.empty());
    for (const char* name :
         {"/help", "/clear", "/compress", "/undo", "/undo!", "/restore", "/wish", "/wish cancel",
          "/end_task", "/approval", "/approval clear", "/workspace", "/skill"})
        CHECK_MESSAGE(hasName(cmds, name), name);
}

TEST_CASE("help_text: names are unique and start with a slash") {
    std::set<std::string> seen;
    for (const auto& c : help_text::commands()) {
        CHECK(c.name.rfind("/", 0) == 0);
        CHECK_MESSAGE(seen.insert(c.name).second, c.name);
    }
}

TEST_CASE("help_text: the menu and the table are the same list") {
    const auto menu = slash_menu::builtins();
    const auto& table = help_text::commands();
    REQUIRE(menu.size() == table.size());
    for (size_t i = 0; i < menu.size(); ++i) {
        // Order matters: it is the display order of the empty "/" filter.
        CHECK(menu[i].name == table[i].name);
        CHECK(menu[i].args == table[i].args);
        // The description is the one field the menu may shorten.
        CHECK(menu[i].desc.size() <= 51);
    }
}

TEST_CASE("help_text: commandHelp lists every command and stays a valid table") {
    const std::string md = help_text::commandHelp();
    CHECK(md.find("Quick Commands") != std::string::npos);
    for (const auto& c : help_text::commands())
        CHECK_MESSAGE(md.find(c.name) != std::string::npos, c.name);
    // /end_task's placeholder contains a pipe; a raw one would split the row.
    CHECK(md.find("\\|") != std::string::npos);
}

TEST_CASE("help_text: the F1 card stays short and covers the non-obvious") {
    const auto& hints = help_text::quickHints();
    REQUIRE(!hints.empty());
    // A starter card, not the reference: past a screenful it has stopped being a
    // nudge and become the wall of text it replaced.
    CHECK(hints.size() <= 8);
    std::set<std::string> chips;
    for (const auto& h : hints) {
        CHECK(!h.key.empty());
        CHECK(!h.desc.empty());
        chips.insert(h.key);
    }
    // The two discovery paths, and the key whose meaning is not obvious.
    for (const char* k : {"/", "@path", "Esc", "Ctrl+C"})
        CHECK_MESSAGE(chips.count(k) == 1, k);
}
