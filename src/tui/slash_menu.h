// proJV -- the "/" command list shown above the input box (M2.5).
//
// Two pure functions and one table. No FTXUI, no sorting by hash order, no state:
// given the same skills directory this always produces the same bytes, which is
// what the unit test pins down.
//
// The table lists only commands that exist. A menu entry that does nothing when
// picked is worse than a menu without it, so /wish and /end_task are added when
// their handlers land, not before.
#pragma once
#include <string>
#include <vector>

namespace slash_menu {

struct Command {
    std::string name; // "/wish"
    std::string args; // "<description>"; empty = no argument
    std::string desc; // one line, clipped to 48 chars
};

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
