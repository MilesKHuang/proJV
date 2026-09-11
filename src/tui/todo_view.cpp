// proJV TUI -- TODO panel renderer implementation.
#include "todo_view.h"
#include "theme_map.h"
#include "theme_manager.h"

#include <sstream>
#include <string>

namespace todo_view {

using ftxui::Color;
using ftxui::Element;
using ftxui::Elements;

namespace {
// Approximate legacy theme colors (Phase 9 wires these to ThemeColors).
struct Palette {
    Color title;
    Color pending;
    Color done;
    Color inProgress;
    Color open;
    Color empty;
};

Palette g_palette;
const Palette& P = g_palette;

void refreshPalette() {
    const auto& T = ThemeManager::instance().current();
    g_palette.title = theme_map::hexToColor(T.todoTitle);
    g_palette.pending = theme_map::hexToColor(T.todoPending);
    g_palette.done = theme_map::hexToColor(T.todoDone);
    g_palette.inProgress = theme_map::hexToColor(T.todoInProgress);
    g_palette.open = theme_map::hexToColor(T.todoOpen);
    g_palette.empty = theme_map::hexToColor(T.todoEmpty);
}
} // namespace

Element renderTodoPanel(const TodoData& todo) {
    refreshPalette();
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
