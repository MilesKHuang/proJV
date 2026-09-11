// proJV TUI -- terminal bootstrap (UTF-8 codepage + VT mode on Windows)
#pragma once

namespace tui {

// Prepare the console for FTXUI rendering:
//  - Windows: switch codepage to UTF-8 and enable VT processing.
//  - Linux: no-op (UTF-8 locale assumed).
// Must be called before constructing ftxui::ScreenInteractive.
void initTerminal();

} // namespace tui
