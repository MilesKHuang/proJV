// proJV -- Ctrl+C must be handled no matter who owns the keyboard.
//
// FTXUI ends the process for an UNHANDLED CtrlC even when ForceHandleCtrlC(false)
// was called: app.cpp:900 is `if (arg == Event::CtrlC && (!handled ||
// force_handle_ctrl_c_)) RecordSignal(SIGINT);` and the next frame turns that
// record into Signal(SIGABRT) -> Exit(). `handled` is simply the return value of
// the root component's OnEvent.
//
// proJV therefore wraps the whole tree in one CatchEvent that swallows CtrlC.
// This test pins the property that matters -- root->OnEvent(CtrlC) == true -- in
// the worst case: a modal with no focusable child, which is exactly the state in
// which the input box stops receiving keys.
#include "doctest/doctest.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

using namespace ftxui;

TEST_CASE("CtrlC: swallowed even when a dialog without focus owns the keyboard") {
    bool shown = true;
    Component dialog = Renderer([] { return text("nothing focusable here"); });
    auto mainComponent = Renderer([] { return text("main"); });
    Component root = Modal(mainComponent, dialog, &shown);

    // The framing proJV uses: one wrapper around everything.
    root |= CatchEvent([](Event e) { return e == Event::CtrlC; });

    // app.cpp:900 reads this return value; false means "exit the app".
    CHECK(root->OnEvent(Event::CtrlC));
}

TEST_CASE("CtrlC: swallowed while idle with the input focused") {
    bool shown = false;
    std::string text_;
    Component input = Input(&text_, "type");
    auto mainComponent = Renderer(input, [input] { return input->Render(); });
    Component root = Modal(mainComponent, Renderer([] { return text("dlg"); }), &shown);

    root |= CatchEvent([](Event e) { return e == Event::CtrlC; });
    CHECK(root->OnEvent(Event::CtrlC));
    CHECK(text_.empty()); // not inserted as a 0x03 byte either
}
