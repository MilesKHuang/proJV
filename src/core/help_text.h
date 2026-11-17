// proJV -- the one command table.
//
// Three readers share it: the "/" completion menu (tui/slash_menu), the /help
// message in chat (core/agent) and the F1 panel (tui/main_tui). Before this file
// the table existed twice and the two copies had already drifted -- /approval and
// /workspace were implementable and implemented, but /help never mentioned them.
//
// Lives in proJV_lib, so it must not include tui/slash_menu.h (the lib's include
// paths do not carry src/tui). The types are defined here and tui/slash_menu.h
// aliases them.
#pragma once

#include <string>
#include <vector>

namespace help_text {

// One "/" command. Field names match what the menu renders; `args` is the
// placeholder shown after the name ("" = the command takes none).
struct CmdHint {
    std::string name; // "/wish"
    std::string args; // "<what you want built>"; empty = no argument
    std::string desc; // one line, no trailing punctuation
};

// One keyboard shortcut, for the F1 panel.
struct KeyHint {
    std::string key;  // "F4"
    std::string desc; // "open session"
};

// The single table, in display order: the commands a user needs most come first,
// because that is the order the empty "/" filter shows.
const std::vector<CmdHint>& commands();

// F1's starter card: only what the hint bar at the bottom cannot show -- the "/"
// menu, "@" attachments, the three meanings of Esc, and the couple of commands
// worth knowing before reading the reference. Deliberately short: the full key
// table lives in docs/*/user-guide/08-reference.md, and /help prints every
// command. This is a nudge, not a manual.
const std::vector<KeyHint>& quickHints();

// Markdown block used by /help and by F1. Generated from commands(), so it cannot
// disagree with the menu; the reference manual is still hand written (AGENTS.md
// forbids generating docs) and is checked by eye at release time.
std::string commandHelp();

} // namespace help_text
