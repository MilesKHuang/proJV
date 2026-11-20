// proJV -- bounded-exit decision without a deadline (see worker_wait.h).
#include "worker_wait.h"

namespace worker_wait {

bool tryJoin(std::thread& thread, Latch& latch) {
    if (!thread.joinable()) return true;
    if (!latch.finished()) {
        // Still running: walk away. Detaching keeps the process alive (a joinable
        // thread destroyed at exit calls std::terminate) and leaves the thread to be
        // reaped by the OS when the process ends.
        thread.detach();
        return false;
    }
    thread.join();
    return true;
}

} // namespace worker_wait
