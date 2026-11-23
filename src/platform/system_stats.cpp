// proJV -- the arithmetic behind ISystemStats, shared by both platforms.
//
// Only the unit rules live here; the reading is platform code. Kept in its own
// translation unit (not inline in the header) so the unit test links the same
// definition the two platform files compile against.
#include "isystem_stats.h"

#include <algorithm>

double cpuPercentBetween(double beforeSeconds, double nowSeconds, double elapsedSeconds, int cores) {
    if (!(elapsedSeconds > 0.0)) return 0.0;
    const double used = std::max(0.0, nowSeconds - beforeSeconds);
    const int count = cores > 0 ? cores : 1;
    const double percent = 100.0 * used / elapsedSeconds / static_cast<double>(count);
    if (!(percent > 0.0)) return 0.0;
    return std::min(100.0, percent);
}
