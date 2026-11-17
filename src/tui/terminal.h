// proJV TUI -- terminal bootstrap (UTF-8 codepage + VT mode on Windows)
#pragma once

namespace tui {

// Prepare the console for FTXUI rendering:
//  - Windows: switch codepage to UTF-8, enable VT processing, enable VT input and
//    leave the mouse to the hosting terminal. Idempotent: main_tui calls it once
//    at startup and again before the loop, because starting child processes
//    (python detection, skill setup, MCP handshakes) can reset the input mode.
//  - Linux: no-op (UTF-8 locale assumed).
// Must be called before constructing ftxui::ScreenInteractive.
void initTerminal();

// Short system beep when a turn finishes and the app returns to idle.
// Windows: MessageBeep. Linux: terminal bell.
void beepIdle();

} // namespace tui
