// proJV -- leaving without lying to yourself about what was cleaned up.
//
// The rule this file encodes: a blocking wait on another thread is only allowed
// when that thread is KNOWN to have finished. Otherwise the caller walks away --
// no deadline, no "give it five more seconds and hope". A fixed timeout was the
// previous design and it was the wrong shape twice over: it made quitting feel
// random (sometimes instant, sometimes five seconds), and when it expired the old
// code went on to close the database and delete the agent anyway, under a thread
// that might still be using them.
//
// So the exit path asks one question -- "is the worker done?" -- and does exactly
// one of two things:
//   done     -> join it and clean up properly (close the DB, checkpoint, reap
//               subprocesses);
//   not done -> cancel it, detach it, and clean up NOTHING. The OS reclaims the
//               address space as the process exits, and SQLite's write-ahead log is
//               replayed on the next start. That is the safe way to walk away.
//
// Killing an in-process thread is not an option anywhere in this: TerminateThread
// does not unwind, does not release the locks the thread holds, and does not run
// destructors. Only a whole process can be killed safely, which is exactly what
// exiting is.
#pragma once

#include <condition_variable>
#include <mutex>
#include <thread>

namespace worker_wait {

// Signalled by the worker as its very last statement.
struct Latch {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;

    void reset() {
        std::lock_guard<std::mutex> lk(mutex);
        done = false;
    }
    void signal() {
        {
            std::lock_guard<std::mutex> lk(mutex);
            done = true;
        }
        cv.notify_all();
    }
    bool finished() {
        std::lock_guard<std::mutex> lk(mutex);
        return done;
    }
};

// If the worker has finished, join it and return true. If it has not, detach it so
// the process can exit without std::terminate, and return false.
// Never blocks: the caller asked "can I clean up safely?", not "please wait".
bool tryJoin(std::thread& thread, Latch& latch);

} // namespace worker_wait
