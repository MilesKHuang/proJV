// proJV studio -- turn clock arithmetic (see work_clock.h).
#include "work_clock.h"

namespace studio {
namespace work_clock {
namespace {

void pushSample(Clock& c, long long nowMs, int tokens) {
    // Trim by time first: the window is about the last seconds, not the last N
    // events, so a fast stream and a slow one are averaged over the same span.
    int first = 0;
    while (first + 1 < c.sampleCount && c.sampleMs[first] < nowMs - kWindowMs)
        ++first;
    if (first > 0) {
        for (int i = first; i < c.sampleCount; ++i) {
            c.sampleMs[i - first] = c.sampleMs[i];
            c.sampleTokens[i - first] = c.sampleTokens[i];
        }
        c.sampleCount -= first;
    }
    if (c.sampleCount >= Clock::kSamples) {
        // Drop the oldest half rather than one: a memmove per frame is cheap, one
        // per sample at 60 fps is cheaper still.
        const int drop = c.sampleCount / 2;
        for (int i = drop; i < c.sampleCount; ++i) {
            c.sampleMs[i - drop] = c.sampleMs[i];
            c.sampleTokens[i - drop] = c.sampleTokens[i];
        }
        c.sampleCount -= drop;
    }
    c.sampleMs[c.sampleCount] = nowMs;
    c.sampleTokens[c.sampleCount] = tokens;
    ++c.sampleCount;
}

} // namespace

void begin(Clock& c, long long nowMs, int completionTokens) {
    if (c.running) return; // a second tick must not restart the turn
    c.running = true;
    c.startedMs = nowMs;
    c.tokensAtStart = completionTokens > 0 ? completionTokens : 0;
    c.sampleCount = 0;
    pushSample(c, nowMs, c.tokensAtStart);
}

void end(Clock& c) {
    c.running = false;
}

double elapsedSeconds(const Clock& c, long long nowMs) {
    if (!c.running) return 0.0;
    const long long delta = nowMs - c.startedMs;
    if (delta <= 0) return 0.0;
    return static_cast<double>(delta) / 1000.0;
}

void noteTokens(Clock& c, long long nowMs, int completionTokens) {
    if (!c.running) return;
    const int value = completionTokens > 0 ? completionTokens : 0;
    if (c.sampleCount > 0 && c.sampleTokens[c.sampleCount - 1] == value) return;
    pushSample(c, nowMs, value);
}

double tokensPerSecond(const Clock& c, long long nowMs) {
    // Find the first sample still inside the window. Reading it here (rather than
    // only on the next sample) is what makes the figure fall to 0 after the model
    // stops talking instead of freezing at its last value.
    int first = 0;
    while (first + 1 < c.sampleCount && c.sampleMs[first] < nowMs - kWindowMs)
        ++first;
    if (c.sampleCount - first < 2) return 0.0;
    const long long span = c.sampleMs[c.sampleCount - 1] - c.sampleMs[first];
    if (span < 300) return 0.0;
    const long long tokens = c.sampleTokens[c.sampleCount - 1] - c.sampleTokens[first];
    if (tokens <= 0) return 0.0;
    return static_cast<double>(tokens) * 1000.0 / static_cast<double>(span);
}

} // namespace work_clock
} // namespace studio
