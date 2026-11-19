// proJV -- deliverable text rules, shared by the skill engine and SubAgent.
//
// One place decides "is this text a report, or just an acknowledgement?". Both
// SkillRunner (which validates the final deliverable) and SubAgent (which may
// swap a bare stop-word for the prose the model already wrote) must agree, or a
// value that one side keeps would be retried by the other.
#pragma once

#include "json.hpp"

#include <cctype>
#include <string>

namespace skill_text {

// "DONE", "done.", "OK" -- the model acknowledging a stop instruction instead of
// writing a report. A stop-word test, NOT a length test: a short but real verdict
// ("ship it") must never be mistaken for an empty deliverable, or every skill
// that ends with a one-line answer would be retried forever.
inline bool isTrivialDeliverable(const std::string& s) {
    std::string t;
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c)))
            t.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    if (t.empty()) return true; // nothing at all
    static const char* kStop[] = {"DONE", "OK",      "FINISHED", "COMPLETE",    "END",
                                  "NONE", "DONEALL", "ALLDONE",  "DONEALLFILES"};
    for (const char* k : kStop)
        if (t == k) return true;
    return false;
}

// Text worth keeping as a deliverable: not a stop-word, and long enough to be
// prose rather than a two-word acknowledgement.
inline bool isSubstantive(const std::string& s) {
    return s.size() >= 40 && !isTrivialDeliverable(s);
}

inline std::string firstLine(const std::string& s) {
    const size_t e = s.find_first_of("\r\n");
    const std::string line = (e == std::string::npos) ? s : s.substr(0, e);
    return line.size() > 40 ? line.substr(0, 40) + "..." : line;
}

// True when every string value carried by a verb call's arguments is a stop-word,
// i.e. the call reports nothing: emit(summary="DONE"). A call with no string value
// at all counts as trivial too.
inline bool payloadIsTrivial(const std::string& jsonArgs) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(jsonArgs);
    } catch (...) {
        return isTrivialDeliverable(jsonArgs);
    }
    if (!j.is_object()) return isTrivialDeliverable(jsonArgs);
    bool sawText = false;
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (!it.value().is_string()) continue;
        sawText = true;
        if (!isTrivialDeliverable(it.value().get<std::string>())) return false;
    }
    (void)sawText;
    return true;
}

} // namespace skill_text
