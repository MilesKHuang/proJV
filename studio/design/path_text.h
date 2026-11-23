// proJV studio -- the text a path becomes when it is sent to the prompt.
//
// Two readers for this string: the agent (which hands it to a tool) and the person
// reading the receipt. Both need the smallest rule that survives both:
//
//     D:\ws\projV\src\main.py      -> D:\ws\projV\src\main.py
//     D:\ws\my project\a b.md      -> "D:\ws\my project\a b.md"
//     D:\                          -> "D:\\"      (an unescaped trailing
//                                                  backslash would eat the quote)
//
// The `@` form is the prefix proJV already uses for "this file is context"; it
// goes in FRONT of the quoted text, never inside the quotes.
//
// Limits, stated rather than hidden: this is not a shell parser. Windows forbids
// `"` in a file name, so a quote inside a path is impossible there; a tab is
// treated like a space; a trailing backslash is doubled, which is what a C-runtime
// parser expects. ImGui-free, so the four cases above are unit tests.
#pragma once

#include <string>

namespace studio {
namespace path_text {

// Does this path survive being pasted without quotes?
bool needsQuotes(const std::string& path);

// The path as one argument, quoted only when it has to be.
std::string quotePath(const std::string& path);

// What gets written into the prompt: `path`, `@path`, or "" for an empty path.
std::string payload(const std::string& path, bool atPrefix);

} // namespace path_text
} // namespace studio
