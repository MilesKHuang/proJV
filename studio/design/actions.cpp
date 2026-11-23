// proJV studio -- the header's button table (see actions.h).
#include "actions.h"

#include <algorithm>

namespace studio {
namespace actions {

const std::vector<Item>& header() {
    // The shortcut strings are the contract with docs/*/user-guide/08-reference.md
    // and with App::handleKey; the test compares the whole set.
    static const std::vector<Item> table = {
        {Id::Role, "Role", "Tab"},
        {Id::NewChat, "New", "F3"},
        {Id::Sessions, "Sessions", "F4"},
        {Id::Model, "Model", "F10"},
        {Id::Reasoning, "Reasoning", "F8"},
        {Id::Dock, "Dock", "F9"},
        {Id::Theme, "Theme", "F6"},
        {Id::ThemeEditor, "Colors", "F7"},
        {Id::Mcp, "MCP", "F5"},
        {Id::Config, "Config", "F2"},
        {Id::Help, "Help", "F1"},
    };
    return table;
}

std::string label(Id id, const State& st) {
    switch (id) {
    case Id::Role:
        // The chip carries the prompt file's own name, and says so: "Role: proJV"
        // cannot be mistaken for the phase pill next to the wordmark.
        return st.role.empty() ? std::string("Role") : "Role: " + st.role;
    case Id::Reasoning:
        return st.reasoning ? "Reasoning on" : "Reasoning off";
    case Id::Dock:
        return st.dock ? "Dock on" : "Dock off";
    default:
        break;
    }
    for (const Item& it : header()) {
        if (it.id == id) return it.label;
    }
    return std::string();
}

std::string tooltip(Id id, const State& st) {
    std::string key;
    for (const Item& it : header()) {
        if (it.id == id) key = it.shortcut;
    }
    std::string what;
    switch (id) {
    case Id::Role:
        what = st.promptFiles > 1
                   ? "cycle the prompt file (" + std::to_string(st.promptFiles) + " installed)"
                   : "the active prompt file";
        break;
    case Id::NewChat:
        what = "start a new chat (the current session is kept)";
        break;
    case Id::Sessions:
        what = "open a saved session";
        break;
    case Id::Model:
        what = "switch model";
        break;
    case Id::Reasoning:
        what = st.reasoning ? "hide the reasoning chain" : "reveal the reasoning chain";
        break;
    case Id::Dock:
        what = st.dock ? "hide the TODO dock" : "show the TODO dock";
        break;
    case Id::Theme:
        what = "switch theme";
        break;
    case Id::ThemeEditor:
        what = "edit one colour of the active theme";
        break;
    case Id::Mcp:
        what = "MCP servers: enable, disable, restart";
        break;
    case Id::Config:
        what = "configuration (model, approval mode, paths)";
        break;
    case Id::Help:
        what = "every key and command";
        break;
    }
    if (key.empty()) return what;
    return key + "  -  " + what;
}

std::vector<std::vector<int>> pack(const std::vector<float>& widths, float gap, float firstRowAvail,
                                   float rowAvail) {
    std::vector<std::vector<int>> rows;
    if (widths.empty()) return rows;
    const float g = gap > 0.0f ? gap : 0.0f;

    std::vector<int> row;
    float used = 0.0f;
    float avail = firstRowAvail;
    for (int i = 0; i < static_cast<int>(widths.size()); ++i) {
        const float w = std::max(0.0f, widths[static_cast<size_t>(i)]);
        const float need = row.empty() ? w : used + g + w;
        // A row that is too narrow for even one control still has to hold it: an
        // empty row followed by the same failure is an infinite loop.
        const bool fits = row.empty() || need <= avail;
        if (!fits) {
            rows.push_back(row);
            row.clear();
            avail = rowAvail;
            used = w;
            row.push_back(i);
            continue;
        }
        used = need;
        row.push_back(i);
    }
    if (!row.empty()) rows.push_back(row);
    return rows;
}

} // namespace actions
} // namespace studio
