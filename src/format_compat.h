// format_compat.h -- minimal snprintf wrapper as std::format replacement
#pragma once
#include <cstdio>
#include <string>
#include <cstdarg>

namespace projv {
    inline std::string fmt(const char* f, ...) {
        char buf[512];
        va_list args;
        va_start(args, f);
        std::vsnprintf(buf, sizeof(buf), f, args);
        va_end(args);
        return buf;
    }
}
