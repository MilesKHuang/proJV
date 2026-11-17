// proJV TUI -- clipboard helper.
//
// Two directions, both UTF-8 at the boundary so CJK survives the round trip:
//   getText() -- used by Ctrl+V (see main_tui's input handler). conhost only
//                translates Ctrl+V into a paste while it owns the input mode;
//                with ENABLE_PROCESSED_INPUT cleared the key arrives as a raw
//                0x16, so proJV does the paste itself.
//   setText() -- nothing calls it yet (kept for a future copy shortcut; the hard
//                part, UTF-8 <-> UTF-16, is what it exists for).
//
// On Linux Ctrl+V is the terminal's job, so getText() reports "unavailable"
// instead of shelling out to xclip for a read.
#pragma once

#include <string>

namespace clipboard {

void setText(const std::string& text);

// Read the clipboard as UTF-8. Returns false when the clipboard is empty, holds
// no text, or the platform has no clipboard read path (Linux).
bool getText(std::string& out);

} // namespace clipboard
