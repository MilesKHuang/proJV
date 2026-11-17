// proJV -- the "/" command list shown above the input box (M2.5).
//
// Two pure functions and one table. No FTXUI, no sorting by hash order, no state:
// given the same skills directory this always produces the same bytes, which is
// what the unit test pins down.
//
// The table itself lives in core/help_text.h, because the chat /help message and
// the F1 panel read the same entries. Including it here (instead of copying the
// entries) is what stops the three readers from drifting apart again.
#pragma once
#include <string>
#include <vector>

#include "core/help_text.h"

namespace slash_menu {

// The menu entry IS the help entry: one struct, one order, no conversion that
// could reorder or drop a field.
using Command = help_text::CmdHint;

// Commands compiled into the binary. Order is the display order for an empty
// filter: the ones a user needs most first.
std::vector<Command> builtins();

// builtins() + one entry per installed skill ("/<folder> <topic>").
// A skill's description comes from its config.json; a broken or missing config
// yields an empty description rather than dropping the skill.
std::vector<Command> all(const std::vector<std::string>& installedSkills,
                         const std::string& skillsDir);

// Filter for the text in the input box (leading "/", no spaces yet).
//   1. prefix matches first ("/w" -> "/wish")
//   2. substring matches after ("/w" -> "/end_task" if it contains "w")
//   3. alphabetical inside each group, so two calls are byte-identical
// An input that is not a "/word" (empty, has a space, no slash) returns nothing.
std::vector<Command> filter(const std::vector<Command>& all, const std::string& input);

// The line the input box gets when a command is picked: the name plus a trailing
// space when it takes arguments, so the cursor lands where the user must type.
std::string completion(const Command& c);

} // namespace slash_menu
