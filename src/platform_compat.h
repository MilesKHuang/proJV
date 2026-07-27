// proJV -- MSVC/GCC compatibility shims
// Provides portable aliases for MSVC-specific CRT functions.
#pragma once

#ifndef _MSC_VER
    #include <cstring>
    #include <ctime>

    // strncpy_s(dst, size, src, count) -> strncpy(dst, src, count)
    #ifndef strncpy_s
    #define strncpy_s(dst, sz, src, cnt) std::strncpy((dst), (src), (cnt))
    #endif

    // localtime_s(tm, time) -> localtime_r(time, tm)
    #ifndef localtime_s
    inline int localtime_s(struct tm* tm, const time_t* t) {
        return localtime_r(t, tm) ? 0 : 1;
    }
    #endif

    // MAX_PATH (used in Win32-only code, provide fallback)
    #ifndef MAX_PATH
    #define MAX_PATH 260
    #endif
#endif
