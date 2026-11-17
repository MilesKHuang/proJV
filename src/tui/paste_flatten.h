// proJV TUI -- paste detection.
//
// A paste arrives as a burst of key events whose gaps are microseconds; a human
// types at >= 32 ms per character (the OS key-repeat ceiling). This unit answers
// two questions only -- "are we inside a burst?" and "should this Return become
// a space?" -- so it touches neither FTXUI nor string boundaries.
#pragma once

#include <chrono>
#include <cstddef>

namespace paste_flatten {

// Tuned here and nowhere else: 5 ms is about 1/6 of the fastest OS key repeat.
constexpr std::chrono::milliseconds kBurstGap{5};
// A paste longer than this is dropped, not flattened: an accidental multi-MB
// paste must not turn into a multi-MB input line.
constexpr size_t kMaxFlattenChars = 200000;

// Feed one timestamp per input event. Returns true when this event arrived less
// than kBurstGap after the previous one, i.e. it belongs to a paste. The first
// call always returns false: a single keystroke is never a burst.
class BurstWindow {
  public:
    bool tick(std::chrono::steady_clock::time_point now);
    void reset();

  private:
    std::chrono::steady_clock::time_point last_{};
    bool hasLast_ = false;
};

// True when a Return must be rewritten into a space: only inside a burst and
// only while the input still has room.
bool shouldFlattenReturn(bool inBurst, size_t currentInputSize, size_t maxChars = kMaxFlattenChars);

} // namespace paste_flatten
