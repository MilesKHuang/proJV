// proJV studio -- the clock behind "how long has this turn been running".
//
// Why this is a studio module and not a view-model field: the view model never
// reads a clock (the same Source must always serialize to the same bytes -- that is
// what makes `--dump-view-model` a golden test), and the TUI shows a turn's length
// nowhere. So elapsed time and throughput are chrome, like the receipt line.
//
// The rate is an average over a short window of *output* tokens, recorded only when
// the count actually moved. Dividing the turn's total by its length was wrong twice
// over: the total included the PROMPT (a 30k-token context divided by the first
// second reads as "30000 tok/s"), and the average never forgot a burst.
//
// Injected timestamps keep it testable: begin/noteTokens/tokensPerSecond with made
// up milliseconds is a statement about arithmetic, not about the wall clock.
#pragma once

namespace studio {
namespace work_clock {

// How far back the rate looks, in milliseconds. Two seconds is long enough to ride
// out the pauses between tool calls and short enough to describe "now".
constexpr long long kWindowMs = 2000;

struct Clock {
    static constexpr int kSamples = 256; // ~4 s at 60 fps; the array is trimmed by time
    bool running = false;
    long long startedMs = 0;      // monotonic ms when the turn started
    long long finalElapsedMs = 0; // duration frozen by end(), 0 until the first stop
    int tokensAtStart = 0;        // OUTPUT tokens when the turn started
    long long sampleMs[kSamples] = {};
    int sampleTokens[kSamples] = {};
    int sampleCount = 0;
};

// Start the clock for a turn. Calling it again while running keeps the first
// start, so a re-entrant tick cannot make a turn look one second old.
void begin(Clock& c, long long nowMs, int completionTokens);

// Stop the clock, freezing the turn's length in finalElapsedMs. Elapsed time stays
// readable after the stop (a finished turn has a duration): the status bar keeps
// showing it until the next begin() starts a new one. A second end() is a no-op, so
// a falling edge that ticks twice cannot overwrite the real duration.
void end(Clock& c, long long nowMs);

// Seconds since the turn started, 0 when it is not running. A finished turn's
// length is Clock::finalElapsedMs -- not this.
double elapsedSeconds(const Clock& c, long long nowMs);

// Record the turn's output-token count. A no-op while the clock is stopped, and a
// no-op when the count has not moved: the window then describes emission, not the
// time spent waiting for the model.
void noteTokens(Clock& c, long long nowMs, int completionTokens);

// Output tokens per second over the samples still inside the window, 0 until two
// samples at least 300 ms apart exist (a rate over a fraction of a second is noise,
// and it renders as a wild number) and 0 once the window has gone quiet.
double tokensPerSecond(const Clock& c, long long nowMs);

} // namespace work_clock
} // namespace studio
