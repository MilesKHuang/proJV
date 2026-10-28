// proJV -- follow-up turn requests raised by a tool.
//
// Two tools end the model's turn and ask the runtime for a turn of their own:
//   run_skill  -> a skill phase, then a nudge turn so the model can report it;
//   view_image -> a synthetic user message carrying the images, then a turn.
//
// Both piggyback on ToolRegistry::requestTurnEnd(), so only ONE of them may be
// pending per tool round: the runtime loop checks them in a fixed order and the
// second request would be silently dropped. This tiny state machine makes the
// rule explicit and testable instead of duplicating two optionals' checks.
#pragma once

namespace turn_requests {

enum class Kind { None, Skill, Image };

inline const char* name(Kind k) {
    switch (k) {
    case Kind::Skill:
        return "run_skill";
    case Kind::Image:
        return "view_image";
    case Kind::None:
    default:
        return "none";
    }
}

struct State {
    Kind pending = Kind::None;

    // Take the slot. False means another request already owns this turn.
    bool claim(Kind k) {
        if (k == Kind::None) return false;
        if (pending != Kind::None) return false;
        pending = k;
        return true;
    }

    // Hand the slot back once the runtime has consumed it (or the turn was
    // cancelled), so the next tool round can ask again.
    void release() { pending = Kind::None; }

    bool is(Kind k) const { return pending == k; }
    bool busy() const { return pending != Kind::None; }

    // Message for a refused caller: names the request that got there first.
    const char* refusedMessage() const {
        switch (pending) {
        case Kind::Skill:
            return "run_skill is already queued for this turn; view_image must be called "
                   "alone, in a turn of its own";
        case Kind::Image:
            return "view_image is already queued for this turn; run_skill must be called "
                   "alone, in a turn of its own";
        case Kind::None:
        default:
            return "another follow-up turn is already queued";
        }
    }
};

} // namespace turn_requests
