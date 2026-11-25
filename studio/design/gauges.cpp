// proJV studio -- resource gauge arithmetic (see gauges.h).
#include "gauges.h"

#include <cmath>
#include <cstdio>

namespace studio {
namespace gauges {

std::string humanBytes(unsigned long long bytes) {
    char buf[48];
    const double value = static_cast<double>(bytes);
    if (bytes >= 1024ull * 1024ull * 1024ull) {
        std::snprintf(buf, sizeof buf, "%.1f GB", value / (1024.0 * 1024.0 * 1024.0));
        return buf;
    }
    if (bytes >= 1024ull * 1024ull) {
        std::snprintf(buf, sizeof buf, "%.0f MB", value / (1024.0 * 1024.0));
        return buf;
    }
    std::snprintf(buf, sizeof buf, "%llu KB", bytes / 1024ull);
    return buf;
}

std::string percentLabel(const std::optional<double>& percent) {
    if (!percent.has_value()) return "-";
    const double v = *percent;
    if (!std::isfinite(v)) return "-";
    const double clamped = v < 0.0 ? 0.0 : (v > 100.0 ? 100.0 : v);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%d%%", static_cast<int>(std::lround(clamped)));
    return buf;
}

std::string memoryLabel(const std::optional<double>& percent) {
    if (!percent.has_value()) return "-";
    const double v = *percent;
    if (!std::isfinite(v)) return "-";
    const double clamped = v < 0.0 ? 0.0 : (v > 100.0 ? 100.0 : v);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%.1f%%", clamped);
    return buf;
}

double fraction(double value, double ceiling) {
    if (!(ceiling > 0.0)) return 0.0;
    if (!(value > 0.0)) return 0.0;
    const double f = value / ceiling;
    return f > 1.0 ? 1.0 : f;
}

double ceilingFor(double value, double minimum) {
    const double floorValue = minimum > 0.0 ? minimum : 1.0;
    return value > floorValue ? value : floorValue;
}

} // namespace gauges
} // namespace studio
