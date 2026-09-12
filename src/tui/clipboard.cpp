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
    // Convert UTF-8 to UTF-16 so CJK text survives the clipboard round-trip.
    int wlen = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (wlen > 0) {
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(wlen) * sizeof(wchar_t));
        if (h) {
            void* p = GlobalLock(h);
            if (p) {
                MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1,
                                    static_cast<wchar_t*>(p), wlen);
                GlobalUnlock(h);
            }
            SetClipboardData(CF_UNICODETEXT, h);
        }
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
