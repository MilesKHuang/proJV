// proJV TUI -- TODO panel renderer (1:1 with legacy renderTodoPanel).
#pragma once

#include "tools/todo_tool.h"

#include <ftxui/dom/elements.hpp>

namespace todo_view {

// Render the TODO panel (title + pendingTodo lines) into an FTXUI element.
ftxui::Element renderTodoPanel(const TodoData& todo);

} // namespace todo_view
