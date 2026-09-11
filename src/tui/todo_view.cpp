// proJV TUI -- TODO panel renderer implementation.
#include "todo_view.h"

#include <sstream>
#include <string>

namespace todo_view {

using ftxui::Color;
using ftxui::Element;
using ftxui::Elements;

namespace {
// Approximate legacy theme colors (Phase 9 wires these to ThemeColors).
struct Palette {
    Color title = Color::RGB(224, 200, 96);
    Color pending = Color::RGB(160, 160, 184);
    Color done = Color::RGB(64, 200, 128);
    Color inProgress = Color::RGB(224, 136, 48);
    Color open = Color::RGB(104, 104, 136);
    Color empty = Color::RGB(56, 56, 88);
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
