// proJV TUI -- terminal bootstrap implementation
#include "terminal.h"

#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#endif

namespace tui {

void initTerminal() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (hOut != INVALID_HANDLE_VALUE && GetConsoleMode(hOut, &mode)) {
        mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        SetConsoleMode(hOut, mode);
    }

    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    if (hIn != INVALID_HANDLE_VALUE && GetConsoleMode(hIn, &mode)) {
        mode |= ENABLE_VIRTUAL_TERMINAL_INPUT; // 0x0200, not VT *processing*
        // ENABLE_PROCESSED_INPUT deliberately stays ON. Clearing it does turn ^C
        // into an ordinary byte, but it takes the console's processed-input
        // conveniences with it: the host's "Ctrl+C copies the selection" and
        // "Ctrl+V pastes" live on that path. Ctrl+C is made harmless without
        // touching the mode instead: system_util_win answers CTRL_C_EVENT and
        // CTRL_BREAK_EVENT with TRUE, and main_tui installs SIG_IGN after FTXUI's
        // own SIGINT handler (see the first-frame block).
        // Mouse stays with the hosting terminal: a selection only works while the
        // console owns MOUSE_EVENT, and FTXUI's Windows reader drops those records
        // anyway (screen_interactive.cpp handles KEY_EVENT only).
        mode &= ~ENABLE_MOUSE_INPUT;
        SetConsoleMode(hIn, mode);
    }
#else
    // Linux: UTF-8 locale assumed. Nothing to do.
#endif
}

void beepIdle() {
#ifdef _WIN32
    MessageBeep(0xFFFFFFFFu); // MB_OK default sound; non-blocking
#else
    std::fputc('\a', stderr); // best-effort terminal bell
    std::fflush(stderr);
#endif
}

} // namespace tui
