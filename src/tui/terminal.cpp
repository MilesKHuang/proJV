// proJV TUI -- terminal bootstrap implementation
#include "terminal.h"

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
        mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        SetConsoleMode(hIn, mode);
    }
#else
    // Linux: UTF-8 locale assumed. Nothing to do.
#endif
}

} // namespace tui
