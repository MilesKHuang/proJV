#pragma once

// --- Loguru integration -------------------------------------------
// This header replaces the old debug_log.h with LogGuru logging.
// Loguru provides: timestamps, levels, file output, thread safety.
//
// Usage:
//   #include "debug_log.h"
//   LOG_F(INFO, "hello %d", 42);
//   LOG_F(WARNING, "something fishy");
//   LOG_F(ERROR, "failed: %s", err.c_str());
//   DLOG_F(INFO, "debug only");
//   VLOG_F(1, "verbose level 1");
//
// Compatibility shims (existing code):
//   debugLog(msg)          → LOG_F(INFO, "%s", (msg).c_str())
//   debugLogf(fmt, ...)    → LOG_F(INFO, fmt, __VA_ARGS__)
//   truncateForLog(s, n)   → unchanged (utility)
//   safeForLog(s)          → unchanged (utility)
//   sanitizeUTF8(s)        → unchanged (utility)

#include <loguru.hpp>

// --- Compatibility shims for existing code -------------------------
// Overload: accepts both std::string and const char*
inline const char* _dbg_str(const std::string& s) { return s.c_str(); }
inline const char* _dbg_str(const char* s) { return s; }

#ifdef PROJV_RELEASE
#define debugLog(msg)
#define debugLogf(fmt, ...)
#else
#ifndef debugLog
#define debugLog(msg)        LOG_F(INFO, "%s", _dbg_str(msg))
#endif

#ifndef debugLogf
#define debugLogf(fmt, ...)  LOG_F(INFO, fmt, ##__VA_ARGS__)
#endif
#endif

// --- Utility: truncate a string for logging ------------------------
#include <string>
#include <cstdio>

inline std::string truncateForLog(const std::string& s, size_t maxLen = 500) {
    if (s.size() <= maxLen) return s;
    return s.substr(0, maxLen) + "\n... [truncated " + std::to_string(s.size() - maxLen) + " bytes]";
}

// --- Utility: replace non-printable chars for safe logging ---------
inline std::string safeForLog(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c >= 32 && c < 127) out += c;
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\x%02x", (unsigned char)c);
            out += buf;
        }
    }
    return out;
}

// --- UTF-8 sanitizer: replace invalid UTF-8 bytes with U+FFFD -----
// Use this before assigning strings to nlohmann::json, which is strict
// about valid UTF-8 and throws type_error.316 on invalid bytes.
inline std::string sanitizeUTF8(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            out += c; ++i;
        } else if (c < 0xC0) {
            out += "\xEF\xBF\xBD"; ++i;
        } else if (c < 0xE0) {
            if (c >= 0xC2 && i + 1 < s.size()
                && ((unsigned char)s[i+1] & 0xC0) == 0x80) {
                out += s[i]; out += s[i+1]; i += 2;
            } else {
                out += "\xEF\xBF\xBD"; ++i;
            }
        } else if (c < 0xF0) {
            if (i + 2 < s.size()
                && ((unsigned char)s[i+1] & 0xC0) == 0x80
                && ((unsigned char)s[i+2] & 0xC0) == 0x80) {
                unsigned char b2 = (unsigned char)s[i+1];
                bool ok = true;
                if (c == 0xE0 && b2 < 0xA0) ok = false;
                if (c == 0xED && b2 >= 0xA0) ok = false;
                if (ok) {
                    out += s[i]; out += s[i+1]; out += s[i+2]; i += 3;
                } else { out += "\xEF\xBF\xBD"; ++i; }
            } else { out += "\xEF\xBF\xBD"; ++i; }
        } else if (c < 0xF8) {
            if (i + 3 < s.size()
                && ((unsigned char)s[i+1] & 0xC0) == 0x80
                && ((unsigned char)s[i+2] & 0xC0) == 0x80
                && ((unsigned char)s[i+3] & 0xC0) == 0x80) {
                unsigned char b2 = (unsigned char)s[i+1];
                bool ok = true;
                if (c == 0xF0 && b2 < 0x90) ok = false;
                if (c == 0xF4 && b2 >= 0x90) ok = false;
                if (c > 0xF4) ok = false;
                if (ok) {
                    out += s[i]; out += s[i+1]; out += s[i+2]; out += s[i+3]; i += 4;
                } else { out += "\xEF\xBF\xBD"; ++i; }
            } else { out += "\xEF\xBF\xBD"; ++i; }
        } else {
            out += "\xEF\xBF\xBD"; ++i;
        }
    }
    return out;
}
