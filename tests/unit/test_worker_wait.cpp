// proJV tests -- how the exit path decides (src/core/worker_wait.h).
//
// The rule under test is the one that replaced a fixed timeout: the caller never
// waits on an unknown thread. A finished worker is joined and the caller may clean
// up; an unfinished one is detached and the caller must clean up nothing.
#include "doctest.h"

#include "core/worker_wait.h"

#include <atomic>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;

TEST_CASE("worker_wait: a finished thread is joined, so cleanup is safe") {
    worker_wait::Latch latch;
    std::thread t([&latch] {
        latch.signal();
    });
    // Wait for the worker to reach its last statement without a deadline loop: the
    // latch is what the caller polls, and it is set by the worker itself.
    while (!latch.finished()) std::this_thread::sleep_for(1ms);
    CHECK(worker_wait::tryJoin(t, latch));
    CHECK_FALSE(t.joinable());
}

TEST_CASE("worker_wait: an unfinished thread is detached, and the call returns at once") {
    worker_wait::Latch latch;
    std::atomic<bool> release{false};
    std::thread t([&latch, &release] {
        while (!release.load()) std::this_thread::sleep_for(2ms);
        latch.signal();
    });
    const auto start = std::chrono::steady_clock::now();
    CHECK_FALSE(worker_wait::tryJoin(t, latch));
    const auto waited = std::chrono::steady_clock::now() - start;
    CHECK(waited < 50ms);      // no deadline was waited out: the answer is immediate
    CHECK_FALSE(t.joinable()); // detached, so nothing tries to join it later
    release.store(true);
    std::this_thread::sleep_for(20ms);
}

TEST_CASE("worker_wait: a latch that was never signalled reads as unfinished") {
    worker_wait::Latch latch;
    CHECK_FALSE(latch.finished());
    latch.signal();
    CHECK(latch.finished());
}

TEST_CASE("worker_wait: reset makes a reused latch read as unfinished again") {
    worker_wait::Latch latch;
    latch.signal();
    latch.reset();
    CHECK_FALSE(latch.finished());
}

TEST_CASE("worker_wait: a non-joinable thread reports success") {
    worker_wait::Latch latch;
    std::thread t;
    CHECK(worker_wait::tryJoin(t, latch));
}
