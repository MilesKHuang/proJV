// proJV -- paste burst detection (src/tui/paste_flatten.h).
//
// Timing and booleans only: the unit never touches a string, so there is no UTF-8
// boundary to test here. What matters is the window's edge behaviour and the fact
// that an oversized paste stops flattening instead of growing forever.
#include "doctest/doctest.h"

#include "tui/paste_flatten.h"

#include <chrono>

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

} // namespace

TEST_CASE("BurstWindow: the first event is never a burst") {
    paste_flatten::BurstWindow window;
    CHECK_FALSE(window.tick(Clock::now()));
}

TEST_CASE("BurstWindow: 1 ms is inside a paste, 100 ms is not") {
    paste_flatten::BurstWindow window;
    const auto t0 = Clock::now();
    CHECK_FALSE(window.tick(t0)); // first event: a keystroke
    CHECK(window.tick(t0 + milliseconds(1)));
    CHECK_FALSE(window.tick(t0 + milliseconds(101)));
}

TEST_CASE("BurstWindow: reset forgets the previous event") {
    paste_flatten::BurstWindow window;
    const auto t0 = Clock::now();
    CHECK_FALSE(window.tick(t0));
    window.reset();
    CHECK_FALSE(window.tick(t0 + milliseconds(1)));
}

TEST_CASE("shouldFlattenReturn: only inside a burst and only while there is room") {
    CHECK(paste_flatten::shouldFlattenReturn(true, 0));
    CHECK_FALSE(paste_flatten::shouldFlattenReturn(false, 0));
    CHECK(paste_flatten::shouldFlattenReturn(true, 199999));
    CHECK_FALSE(paste_flatten::shouldFlattenReturn(true, 200000));
    CHECK_FALSE(paste_flatten::shouldFlattenReturn(false, 300000));
}
