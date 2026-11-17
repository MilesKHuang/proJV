// proJV -- the Modal focus trap that broke the F1 help panel.
//
// FTXUI routes a key to a modal only when the modal's subtree is Focused(), and
// Focused() requires a focusable descendant (see ComponentBase::Focused() and
// ContainerBase::OnEvent in external/FTXUI-7.0.3). A dialog built from a bare
// Renderer therefore swallows every key -- and because the modal is also the
// active tab, the rest of the UI stops receiving keys too.
//
// That is exactly what the first F1 panel did: it could be opened and never
// closed, and the only way out was closing the window. Case 1 pins the rule down,
// case 2 pins the fix. If somebody removes the Close button from the help panel,
// case 2 is the assertion that starts lying.
#include "doctest/doctest.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

using namespace ftxui;

namespace {

// main + modal, the same shape as proJV's modal chain.
Component makeModal(bool* shown, Component dialog) {
    auto mainComponent = Renderer([] { return text("main"); });
    return Modal(mainComponent, std::move(dialog), shown);
}

} // namespace

TEST_CASE("Modal: a dialog without a focusable child never sees the key") {
    bool shown = true;
    int hits = 0;
    Component dialog = Renderer([] { return text("no buttons here"); });
    dialog |= CatchEvent([&](Event e) {
        if (e == Event::Escape) {
            ++hits;
            return true;
        }
        return false;
    });

    auto root = makeModal(&shown, dialog);
    root->OnEvent(Event::Escape);
    CHECK(hits == 0); // the bug the F1 panel shipped with
}

TEST_CASE("Modal: a dialog with one button receives Escape and Return") {
    bool shown = true;
    int escapes = 0;
    Component close = Button("Close", [&] { shown = false; });
    Component dialog = Renderer(close, [&, close] { return vbox({close->Render()}) | border; });
    dialog |= CatchEvent([&](Event e) {
        if (e == Event::Escape) {
            ++escapes;
            return true;
        }
        return false; // Return must reach the focused button
    });

    auto root = makeModal(&shown, dialog);
    CHECK(root->OnEvent(Event::Escape));
    CHECK(escapes == 1);
    CHECK(root->OnEvent(Event::Return));
    CHECK_FALSE(shown); // the button ran
}

TEST_CASE("Modal: a CatchEvent that swallows Return makes every button dead") {
    bool shown = true;
    int clicks = 0;
    Component ok = Button("Quit", [&] { ++clicks; });
    Component dialog = Renderer(ok, [ok] { return ok->Render(); });
    // The trap the quit confirmation shipped with: "Esc should cancel" was written
    // as "Esc or Return closes", so Return never reached the focused button and
    // picking "Quit" only closed the box. Esc-only is the correct shape.
    dialog |= CatchEvent([](Event e) { return e == Event::Escape || e == Event::Return; });
    auto root = makeModal(&shown, dialog);
    CHECK(root->OnEvent(Event::Return));
    CHECK(clicks == 0); // swallowed by the decorator, never delivered
}
