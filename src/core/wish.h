// proJV -- the /wish mode state.
//
// A wish is "I want this built; decide the details yourself and do not stop until
// the TODO list is empty". It is NOT a goal in the CodeWhale sense: there is no
// external verification, the TODO checklist the model wrote IS the definition of
// done, and the user can watch it in the dock.
//
// Deliberately tiny: everything a wish needs (the original request, the TODO, the
// overview) is already injected every turn by Agent::buildChatRequest(). All this
// struct adds is a switch, a pass counter and two stop conditions.
#pragma once
#include <string>

struct Wish {
    bool active = false;
    std::string objective;

    // Completed passes through runAgentTurnLoop while active.
    int passes = 0;
    // Consecutive passes that called no tool at all (the model talked instead of
    // working). Two in a row means it is stuck, not thinking.
    int idleRounds = 0;
    // wish_decide calls recorded under this wish.
    int decisions = 0;

    // Set by the wish_block tool: the model says it cannot continue.
    bool blocked = false;
    std::string blockReason;

    // Internal: any tool call in the current pass sets this. Reset per pass.
    bool progressedThisPass = false;
    // A TODO list was seen at least once. Without it "the TODO is empty" would mean
    // both "finished" and "never started", and every wish would end after one pass.
    bool sawTodo = false;

    void reset(const std::string& objectiveText) {
        active = true;
        objective = objectiveText;
        passes = 0;
        idleRounds = 0;
        decisions = 0;
        blocked = false;
        blockReason.clear();
        progressedThisPass = false;
        sawTodo = false;
    }
    void clear() {
        active = false;
        objective.clear();
        passes = 0;
        idleRounds = 0;
        decisions = 0;
        blocked = false;
        blockReason.clear();
        progressedThisPass = false;
        sawTodo = false;
    }
};

namespace wish_mode {

// Two idle passes in a row: the model produced no tool call twice, so no amount of
// further passes will advance the TODO. Reported with the remaining checklist.
constexpr int kMaxIdleRounds = 2;
// Hard ceiling. 50 passes is roughly a day of file edits; past that the wish is
// almost certainly mis-specified rather than unfinished.
constexpr int kMaxPasses = 50;

} // namespace wish_mode
