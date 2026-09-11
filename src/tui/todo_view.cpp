// proJV TUI -- TODO panel renderer implementation.
#include "todo_view.h"
#include "theme_map.h"

#include <sstream>
#include <string>

namespace todo_view {

using ftxui::Color;
using ftxui::Element;
using ftxui::Elements;

namespace {
// Approximate legacy theme colors (Phase 9 wires these to ThemeColors).
struct Palette {
    Color title = theme_map::hexToColor("#E0C860");
    Color pending = theme_map::hexToColor("#A0A0B8");
    Color done = theme_map::hexToColor("#40C880");
    Color inProgress = theme_map::hexToColor("#E08830");
    Color open = theme_map::hexToColor("#686888");
    Color empty = theme_map::hexToColor("#383858");
};
const Palette P;
} // namespace

Element renderTodoPanel(const TodoData& todo) {
    Elements els;
    els.push_back(ftxui::text("TODO") | ftxui::color(P.title));
    els.push_back(ftxui::separator());

    if (!todo.pendingTodo.empty()) {
        std::istringstream stream(todo.pendingTodo);
        std::string line;
        while (std::getline(stream, line)) {
            Color c = P.pending;
            if (line.find("[x]") != std::string::npos) c = P.done;
            else if (line.find("[*]") != std::string::npos) c = P.inProgress;
            else if (line.find("[ ]") != std::string::npos) c = P.open;
            els.push_back(ftxui::text(line) | ftxui::color(c));
        }
    } else {
        els.push_back(ftxui::text("  (No active tasks)") | ftxui::color(P.empty));
    }

    return ftxui::vbox(std::move(els));
}

} // namespace todo_view
