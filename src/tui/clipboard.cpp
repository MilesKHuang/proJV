// proJV TUI -- clipboard helper implementation.
#include "clipboard.h"

#ifdef _WIN32
#include <windows.h>
#include <cstring>
#endif

#include <cstdio>
#include <string>

namespace clipboard {

void setText(const std::string& text) {
#ifdef _WIN32
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    size_t len = text.size() + 1;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, len);
    if (h) {
        void* p = GlobalLock(h);
        if (p) {
            std::memcpy(p, text.c_str(), len);
            GlobalUnlock(h);
        }
        SetClipboardData(CF_TEXT, h);
    }
    CloseClipboard();
#else
    FILE* f = popen("xclip -selection clipboard", "w");
    if (f) {
        std::fwrite(text.c_str(), 1, text.size(), f);
        pclose(f);
    }
#endif
}

} // namespace clipboard
