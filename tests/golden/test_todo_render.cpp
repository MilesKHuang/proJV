// TODO panel golden test.
#include "doctest.h"

#include "tui/todo_view.h"
#include "tools/todo_tool.h"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include <string>

using namespace ftxui;

static std::string renderTodo(const TodoData& td) {
    auto doc = todo_view::renderTodoPanel(td);
    auto screen = Screen::Create(Dimension::Fixed(40), Dimension::Fixed(12));
    Render(screen, doc);
    return screen.ToString();
}

TEST_CASE("todo_view: empty state") {
    TodoData td;
    CHECK(renderTodo(td).find("(No active tasks)") != std::string::npos);
}

TEST_CASE("todo_view: pending lines with markers") {
    TodoData td;
    td.pendingTodo = "Tasks:\n[x] done task\n[*] in progress\n[ ] open task\nplain task";
    std::string out = renderTodo(td);
    CHECK(out.find("TODO") != std::string::npos);
    CHECK(out.find("done task") != std::string::npos);
    CHECK(out.find("in progress") != std::string::npos);
    CHECK(out.find("open task") != std::string::npos);
    CHECK(out.find("plain task") != std::string::npos);
}
