// proJV studio -- the clock behind "how long has this turn been running".
//
// Why this is a studio module and not a view-model field: the view model never
// reads a clock (the same Source must always serialize to the same bytes -- that is
// what makes `--dump-view-model` a golden test), and the TUI shows a turn's length
// nowhere. So elapsed time and throughput are chrome, like the receipt line.
//
// Injected timestamps keep it testable: `begin(c, 1000, 0)` then
// `tokensPerSecond(c, 3000, 400)` is a statement about arithmetic, not about the
// wall clock.
#pragma once

namespace studio {
namespace work_clock {

struct Clock {
    bool running = false;
    long long startedMs = 0;  // monotonic ms when the turn started
    int tokensAtStart = 0;    // prompt+completion tokens when the turn started
};

// Start the clock for a turn. Calling it again while running keeps the first
// start, so a re-entrant tick cannot make a turn look one second old.
void begin(Clock& c, long long nowMs, int tokens);

// Stop the clock. Elapsed time stays readable (a finished turn has a duration).
void end(Clock& c);

// Seconds since the turn started, 0 when it is not running.
double elapsedSeconds(const Clock& c, long long nowMs);

// Tokens per second over the turn, 0 before one second has passed (a rate over a
// fraction of a second is noise, and it renders as a wild number).
double tokensPerSecond(const Clock& c, long long nowMs, int tokens);

} // namespace work_clock
} // namespace studio
