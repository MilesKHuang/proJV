// proJV studio -- turn clock arithmetic (see work_clock.h).
#include "work_clock.h"

namespace studio {
namespace work_clock {

void begin(Clock& c, long long nowMs, int tokens) {
    if (c.running) return; // a second tick must not restart the turn
    c.running = true;
    c.startedMs = nowMs;
    c.tokensAtStart = tokens > 0 ? tokens : 0;
}

void end(Clock& c) { c.running = false; }

double elapsedSeconds(const Clock& c, long long nowMs) {
    if (!c.running) return 0.0;
    const long long delta = nowMs - c.startedMs;
    if (delta <= 0) return 0.0;
    return static_cast<double>(delta) / 1000.0;
}

double tokensPerSecond(const Clock& c, long long nowMs, int tokens) {
    const double seconds = elapsedSeconds(c, nowMs);
    if (seconds < 1.0) return 0.0;
    const long long used = static_cast<long long>(tokens) - c.tokensAtStart;
    if (used <= 0) return 0.0;
    return static_cast<double>(used) / seconds;
}

} // namespace work_clock
} // namespace studio
