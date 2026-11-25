// proJV studio -- the prompt composer (design 08.5).
//
// The one widget where "the terminal and the window must behave the same" is
// decided key by key: Enter sends (or fills the "/" menu), the text is gone
// afterwards, Up/Down drive the menu, Esc means three things.
//
// Rule this file is built on: the APP's text is the truth, the ImGui buffer is a
// view of it. Every frame that finds them different copies the app's text over
// the buffer. Comparing text with text (instead of trusting a flag) is what stops
// the two classic failures: text left behind after Enter, and a widget that keeps
// showing what a slash-command fill replaced.
#pragma once

namespace studio {

class App;
struct Frame;

// Draw the prompt card inside `frame.composer`. Reads and writes App::inputText().
void drawPrompt(App& app, const Frame& frame);

} // namespace studio
