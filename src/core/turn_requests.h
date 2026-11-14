// proJV -- follow-up turn requests raised by a tool.
//
// Three tools end the model's turn and ask the runtime for a turn of their own:
//   run_skill    -> a skill phase, then a nudge turn so the model can report it;
//   view_image   -> a synthetic user message carrying the images, then a turn;
//   exec_shell(background:true) -> a report once the detached task finishes.
//
// It used to be a single slot: the second request of a tool round was silently
// dropped, and every caller had to explain the refusal to the model. A queue costs
// nothing and removes that whole class of "the skill never ran" bug. Image still
// jumps the queue -- "I cannot continue until I see this picture" must not wait
// behind a five-minute skill.
#pragma once
#include <deque>

namespace turn_requests {

enum class Kind { None, Skill, Image, Process };

struct State {
    std::deque<Kind> pending;

    // Always succeeds (except for Kind::None). Image goes to the front, everything
    // else to the back.
    bool claim(Kind k) {
        if (k == Kind::None) return false;
        if (k == Kind::Image)
            pending.push_front(k);
        else
            pending.push_back(k);
        return true;
    }

    Kind peek() const { return pending.empty() ? Kind::None : pending.front(); }
    void consume() {
        if (!pending.empty()) pending.pop_front();
    }

    // Withdraw one queued request by kind (the caller changed its mind before the
    // runtime got there). Returns false when it was not queued.
    bool drop(Kind k) {
        for (auto it = pending.rbegin(); it != pending.rend(); ++it) {
            if (*it == k) {
                pending.erase(std::next(it).base());
                return true;
            }
        }
        return false;
    }

    bool busy() const { return !pending.empty(); }
    size_t size() const { return pending.size(); }

    // End of turn / new turn: nothing carries over.
    void release() { pending.clear(); }
};

} // namespace turn_requests
