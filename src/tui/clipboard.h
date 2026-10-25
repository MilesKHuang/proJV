// proJV TUI -- clipboard helper (copy message text).
//
// STATUS: implemented but NOT WIRED. Nothing in the tree calls setText(); the
// feature it was written for (copy the last assistant answer from the TUI) was
// never finished -- there is no key binding and no notion of "the selected
// message" yet. Kept, rather than deleted, because the hard part (UTF-8 ->
// UTF-16 on Windows so CJK survives the clipboard round-trip) is done.
//
// To finish it: bind a key in app_tui.cpp, resolve "the last answer" from the
// session, call clipboard::setText(). On Linux this shells out to `xclip`, so a
// missing xclip should be reported to the user rather than ignored.
#pragma once

#include <string>

namespace clipboard {

void setText(const std::string& text);

} // namespace clipboard
