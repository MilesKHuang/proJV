#include "log_files.h"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace log_files {
namespace {

bool matchLogName(const std::string& name, int& outYear, int& outMonth, int& outDay) {
    // Exactly proJV-YYYY-MM-DD.log, nothing else.
    const std::string prefix = "proJV-";
    const std::string suffix = ".log";
    if (name.size() != prefix.size() + 10 + suffix.size()) return false;
    if (name.compare(0, prefix.size(), prefix) != 0) return false;
    if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) return false;
    int y = 0, m = 0, d = 0;
    if (std::sscanf(name.c_str() + prefix.size(), "%4d-%2d-%2d", &y, &m, &d) != 3) return false;
    if (y < 1970 || y > 9999 || m < 1 || m > 12 || d < 1 || d > 31) return false;
    outYear = y;
    outMonth = m;
    outDay = d;
    return true;
}

time_t midnightLocal(int year, int month, int day) {
    std::tm tm = {};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    return std::mktime(&tm);
}

std::string dateOf(time_t t) {
    std::tm tm = {};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

} // namespace

std::string todayLogPath(const std::string& logDir) {
    const std::string day = dateOf(std::time(nullptr));
    if (logDir.empty()) return "proJV-" + day + ".log";
    const char last = logDir.back();
    const std::string sep = (last == '/' || last == '\\') ? "" : "/";
    return logDir + sep + "proJV-" + day + ".log";
}

std::size_t pruneOldLogs(const std::string& logDir, int keepDays) {
    if (keepDays <= 0 || logDir.empty()) return 0;

    std::error_code ec;
    const time_t today = [&] {
        std::tm tm = {};
        const time_t now = std::time(nullptr);
#ifdef _WIN32
        localtime_s(&tm, &now);
#else
        localtime_r(&now, &tm);
#endif
        return midnightLocal(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    }();

    std::size_t removed = 0;
    for (const auto& entry : fs::directory_iterator(logDir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        const std::string name = entry.path().filename().string();
        int y = 0, m = 0, d = 0;
        if (!matchLogName(name, y, m, d)) continue;

        const time_t fileDay = midnightLocal(y, m, d);
        if (fileDay == (time_t)-1) continue; // invalid date such as 2026-02-31
        const double ageDays = std::difftime(today, fileDay) / 86400.0;
        if (ageDays <= static_cast<double>(keepDays)) continue;

        std::error_code rmEc;
        if (fs::remove(entry.path(), rmEc) && !rmEc) ++removed;
    }
    return removed;
}

} // namespace log_files
