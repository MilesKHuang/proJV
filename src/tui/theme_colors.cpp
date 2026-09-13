// proJV TUI -- theme color model implementation.
#include "theme_colors.h"

#include "json.hpp"

#include <string>
#include <vector>

namespace {

// Assign a hex value to every field via the shared field list.
void setAll(ThemeColors& tc, const std::vector<std::string>& values) {
    const auto& fields = themeColorFields();
    for (size_t i = 0; i < fields.size() && i < values.size(); ++i) {
        tc.*fields[i].member = values[i];
    }
}

} // namespace

const std::vector<ThemeColorField>& themeColorFields() {
    static const std::vector<ThemeColorField> fields = {
#define X(n) {#n, &ThemeColors::n},
        PROJV_THEME_FIELDS(X)
#undef X
    };
    return fields;
}

std::string ThemeColors::toJson() const {
    nlohmann::json j;
    j["name"] = name;
    j["author"] = author;
    j["version"] = version;
    nlohmann::json& c = j["colors"];
    for (const auto& f : themeColorFields()) {
        c[f.name] = this->*f.member;
    }
    return j.dump(2);
}

ThemeColors ThemeColors::fromJson(const std::string& json) {
    ThemeColors tc = obsidian();
    try {
        nlohmann::json j = nlohmann::json::parse(json);
        if (j.contains("name") && j["name"].is_string()) tc.name = j["name"].get<std::string>();
        if (j.contains("author") && j["author"].is_string()) tc.author = j["author"].get<std::string>();
        if (j.contains("version") && j["version"].is_string()) tc.version = j["version"].get<std::string>();
        if (j.contains("colors") && j["colors"].is_object()) {
            auto& c = j["colors"];
            for (const auto& f : themeColorFields()) {
                if (c.contains(f.name) && c[f.name].is_string()) {
                    tc.*f.member = c[f.name].get<std::string>();
                }
            }
        }
    } catch (...) {}
    return tc;
}

ThemeColors ThemeColors::obsidian() {
    ThemeColors tc;  // field order matches PROJV_THEME_FIELDS
    setAll(tc, {
        "#121218", "#0C0C12", "#1A1A24", "#D4D4E0", "#585868", "#0C0C12",
        "#0C0C12", "#3A3A50", "#1A1A24", "#28283A", "#3A3A55",
        "#121218", "#28283A", "#28283A", "#161620", "#1A1A24", "#28283A", "#3A3A55",
        "#1E2840", "#1E281E", "#181820", "#14141C", "#2A2418",
        "#E0A040", "#1C1C2A", "#A0A0B8",
        "#181828", "#484880", "#A0A0E0", "#101020", "#9090D0",
        "#686888", "#E0C860", "#40C8A0", "#E06060", "#E03040",
        "#686888", "#686888", "#484868", "#484868", "#A0A848",
        "#E08830", "#E03040", "#C8C040", "#40C880", "#484868",
        "#E08830", "#E06060", "#E03040", "#383858", "#686888",
        "#E0C860", "#A0A0B8", "#40C880", "#224488", "#686888", "#383858",
        "#E03040", "#585868",
        "#E0B840", "#E0A840", "#D0C070", "#E0B840", "#70B8D8", "#E07850", "#6090E0", "#4068C0",
        "#70A8C8", "#303048", "#181828", "#8888A0", "#5858A0", "#1E1E2C",
        "#0A0A10",
    });
    return tc;
}

ThemeColors ThemeColors::light() {
    ThemeColors tc;
    tc.name = "Light";
    // Light statusTokenInfo/ModelName/TokenCount/MsgCount/ToolCount were not
    // recoverable from the deleted source (tool redaction); inferred from the
    // light grayscale palette. Cosmetic only.
    setAll(tc, {
        "#F5F5F5", "#E8E8E8", "#FFFFFF", "#1A1A1E", "#9999A0", "#E0E0E0",
        "#E8E8E8", "#C0C0C8", "#E8E8EC", "#D0D0D8", "#B8B8C0",
        "#F5F5F5", "#D0D0D8", "#D0D0D8", "#FAFAFA", "#EBEBF0", "#D8D8E0", "#C0C0CC",
        "#DCE8F0", "#E2ECD8", "#E8E8EC", "#EEEEF0", "#FFF3CD",
        "#CC7A00", "#EDEDF2", "#555560",
        "#EDE8F8", "#B8A0D8", "#553388", "#F5F0FC", "#664499",
        "#777780", "#CC8800", "#228855", "#CC3333", "#CC0000",
        "#777780", "#777780", "#555560", "#555560", "#889944",
        "#E67800", "#CC0000", "#AAAA33", "#228855", "#555560",
        "#CC7A00", "#CC3333", "#CC0000", "#666670", "#777780",
        "#CC8800", "#555560", "#228855", "#CC8800", "#777780", "#9999A0",
        "#CC0000", "#888890",
        "#CC6600", "#CC7700", "#B89030", "#CC7700", "#3377AA", "#CC5533", "#2266CC", "#1144AA",
        "#4488BB", "#C0C0C8", "#EEEEF2", "#666670", "#4488BB", "#E0E0E8",
        "#F0F0F0",
    });
    return tc;
}
