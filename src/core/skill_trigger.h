// proJV -- "/<skill> <topic>" trigger resolution (pure logic, no UI deps).
//
// Kept out of TuiApp so the dispatch rules can be unit-tested without a
// terminal. sendMessage() calls parse() once per user line.
#pragma once

#include <string>
#include <vector>

namespace skill_trigger {

struct Result {
    enum class Kind {
        None,     // ordinary message -- fall through
        List,     // "/skill" or "/skill list" -> print the installed skills
        Trigger   // "/<installed skill> <topic>" -> run it
    };

    Kind kind = Kind::None;
    std::string name;    // canonical installed folder name (Trigger only)
    std::string topic;   // text after the skill name, trimmed (Trigger only)
};

// True for commands that keep their meaning even when a skill folder shadows
// their name (/clear /help /workspace /compress).
bool isReservedCommand(const std::string& lowerToken);

// Resolve the leading "/<token>" of `input` against `installed`.
// Unknown tokens and reserved commands return Kind::None, so callers never
// have to special-case them: nothing is intercepted, nothing errors.
Result parse(const std::string& input, const std::vector<std::string>& installed);

} // namespace skill_trigger
