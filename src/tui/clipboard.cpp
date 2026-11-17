// proJV TUI -- clipboard helper implementation.
#include "clipboard.h"

#ifdef _WIN32
#include <windows.h>
#include <cstring>
#endif

#include <cstdio>
#include <string>
#include <utility>

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
                MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, static_cast<wchar_t*>(p), wlen);
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

bool getText(std::string& out) {
    out.clear();
#ifdef _WIN32
    if (!OpenClipboard(nullptr)) return false;
    bool ok = false;

    const auto fromUtf16 = [&out](HANDLE h) -> bool {
        const auto* w = static_cast<const wchar_t*>(GlobalLock(h));
        if (!w) return false;
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        bool got = false;
        if (bytes > 1) {
            std::string buf(static_cast<size_t>(bytes), '\0');
            WideCharToMultiByte(CP_UTF8, 0, w, -1, buf.data(), bytes, nullptr, nullptr);
            buf.resize(static_cast<size_t>(bytes - 1)); // drop the terminating NUL
            if (!buf.empty()) {
                out = std::move(buf);
                got = true;
            }
        }
        GlobalUnlock(h);
        return got;
    };

    // CF_TEXT / CF_OEMTEXT are single-byte code pages: a console or an old editor
    // may publish text in the ANSI/OEM format only, and a reader that asks for
    // CF_UNICODETEXT alone then gets nothing (or something stale).
    const auto fromAnsi = [&out](HANDLE h, UINT codepage) -> bool {
        const auto* p = static_cast<const char*>(GlobalLock(h));
        if (!p) return false;
        bool got = false;
        const int wlen = MultiByteToWideChar(codepage, 0, p, -1, nullptr, 0);
        if (wlen > 1) {
            std::wstring w(static_cast<size_t>(wlen), L'\0');
            MultiByteToWideChar(codepage, 0, p, -1, w.data(), wlen);
            w.resize(static_cast<size_t>(wlen - 1));
            const int bytes =
                WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
            if (bytes > 1) {
                std::string buf(static_cast<size_t>(bytes), '\0');
                WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, buf.data(), bytes, nullptr,
                                    nullptr);
                buf.resize(static_cast<size_t>(bytes - 1));
                if (!buf.empty()) {
                    out = std::move(buf);
                    got = true;
                }
            }
        }
        GlobalUnlock(h);
        return got;
    };

    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (fromUtf16(h)) ok = true;
    }
    if (!ok) {
        if (HANDLE h = GetClipboardData(CF_TEXT)) {
            if (fromAnsi(h, CP_ACP)) ok = true;
        }
    }
    if (!ok) {
        if (HANDLE h = GetClipboardData(CF_OEMTEXT)) {
            if (fromAnsi(h, CP_OEMCP)) ok = true;
        }
    }
    CloseClipboard();
    return ok;
#else
    return false; // Ctrl+V is the terminal's job on POSIX
#endif
}

} // namespace clipboard
