// proJV studio -- path quoting for the prompt (see path_text.h).
#include "path_text.h"

namespace studio {
namespace path_text {

namespace {

// Everything that makes a bare path ambiguous for a command line: whitespace, the
// shell's own punctuation, and the two quote characters. Same set as the reference
// implementation this was designed from.
constexpr const char* kUnsafe = " \t\"'`&()[]{}^=;!,|<>$";

const char kAt = '@';

} // namespace

bool needsQuotes(const std::string& path) {
    if (path.empty()) return false;
    if (path.back() == '\\') return true; // the closing quote would be escaped
    for (char c : path) {
        for (const char* u = kUnsafe; *u; ++u) {
            if (c == *u) return true;
        }
    }
    return false;
}

std::string quotePath(const std::string& path) {
    if (path.empty()) return std::string();
    if (!needsQuotes(path)) return path;
    std::string body;
    body.reserve(path.size() + 2);
    for (char c : path) {
        if (c == '"') continue; // impossible on Windows; a stray quote is dropped
        body.push_back(c);
    }
    if (!body.empty() && body.back() == '\\') body.push_back('\\');
    return "\"" + body + "\"";
}

std::string payload(const std::string& path, bool atPrefix) {
    const std::string quoted = quotePath(path);
    if (quoted.empty()) return std::string();
    if (!atPrefix) return quoted;
    return std::string(1, kAt) + quoted;
}

} // namespace path_text
} // namespace studio
