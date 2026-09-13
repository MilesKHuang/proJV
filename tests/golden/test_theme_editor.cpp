// Theme editor render + event regression test: opening F7 must not crash.
#include "doctest.h"

#include "tui/theme_editor_view.h"
#include "tui/theme_manager.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/screen/screen.hpp>

using namespace ftxui;

TEST_CASE("theme editor renders without crash") {
    ThemeManager::instance().init("", "");
    auto editor = theme_editor::makeThemeEditor([] {});
    auto screen = Screen::Create(Dimension::Fixed(100), Dimension::Fixed(30));
    Render(screen, editor->Render());
    CHECK_FALSE(screen.ToString().empty());
}

TEST_CASE("theme menu renders without crash") {
    ThemeManager::instance().init("", "");
    auto menu = theme_editor::makeThemeMenu([] {});
    auto screen = Screen::Create(Dimension::Fixed(60), Dimension::Fixed(20));
    Render(screen, menu->Render());
    CHECK_FALSE(screen.ToString().empty());
}

TEST_CASE("theme editor in modal handles key events") {
    ThemeManager::instance().init("", "");
    auto editor = theme_editor::makeThemeEditor([] {});
    bool show = true;
    auto main = Renderer([] { return text("main"); });
    auto root = Modal(main, editor, &show);

    auto screen = Screen::Create(Dimension::Fixed(120), Dimension::Fixed(30));
    Render(screen, root->Render());

    // Simulate the key events F7 would receive.
    root->OnEvent(Event::Tab);
    root->OnEvent(Event::Return);
    root->OnEvent(Event::ArrowDown);
    root->OnEvent(Event::ArrowUp);
    root->OnEvent(Event::Escape);
    CHECK(true);
}
