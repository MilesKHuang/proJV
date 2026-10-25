// proJV -- "/<skill> <topic>" trigger resolution (see skill_trigger.h).
#include "skill_trigger.h"

#include <cctype>

namespace skill_trigger {
namespace {

std::string toLowerAscii(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string trimCopy(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

} // namespace

bool isReservedCommand(const std::string& lowerToken) {
    static const char* kReserved[] = {"clear", "help", "workspace", "compress"};
    for (const char* k : kReserved)
        if (lowerToken == k) return true;
    return false;
}

Result parse(const std::string& input, const std::vector<std::string>& installed) {
    Result r;
    const std::string text = trimCopy(input);
    if (text.size() < 2 || text[0] != '/') return r;

    const size_t sp = text.find_first_of(" \t\r\n");
    const std::string token = (sp == std::string::npos) ? text.substr(1) : text.substr(1, sp - 1);
    if (token.empty()) return r;

    const std::string rest = (sp == std::string::npos) ? std::string() : text.substr(sp + 1);
    const std::string lower = toLowerAscii(token);

    if (lower == "skill" || lower == "skills") {
        r.kind = Result::Kind::List;
        return r;
    }
    if (isReservedCommand(lower)) return r;

    for (const auto& folder : installed) {
        if (toLowerAscii(folder) == lower) {
            r.kind = Result::Kind::Trigger;
            r.name = folder;
            r.topic = trimCopy(rest);
            return r;
        }
    }
    return r; // unknown token -> ordinary message
}

} // namespace skill_trigger
