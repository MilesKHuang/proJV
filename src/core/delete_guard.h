#pragma once

#include <string>
#include <vector>

namespace delete_guard {

// What the agent should do with one exec_shell command.
//
//   Allow -- run it, no dialog.
//   Ask   -- run it only after the user confirms (see Agent::approveTool).
//   Deny  -- never run it, no dialog; the model gets an error result back.
//
// Deny is reserved for disk formatting/partitioning: those cannot be undone by
// any backup, so there is nothing sensible to ask the user. Everything that
// merely deletes or overwrites user files stays Ask -- the user always has the
// last word there.
enum class Decision { Allow, Ask, Deny };

// What kind of risk an Ask carries. This exists because "needs a dialog" was too
// broad in practice: writing build output with `>` asked exactly like `rm -rf`
// did, so a normal build session turned into a dozen clicks. With auto_approve on
// (the default) only Destructive asks; Functional runs.
//
// Fail-safe default is Destructive: a branch that forgets to set the kind keeps
// asking, which is the conservative direction.
enum class Kind {
    Functional,  // output/overwrite of generated data: `> build/log.txt`, `tee log`
    Destructive, // removes or replaces data that may be the only copy
};

struct Verdict {
    Decision decision = Decision::Allow;
    Kind kind = Kind::Destructive;
    // mv/cp/robocopy style: "ask" only matters when the target already exists.
    // The classifier is pure and cannot stat, so it reports the target and lets
    // the caller downgrade Ask to Allow when the target is new.
    bool needsExistenceCheck = false;
    std::string checkTarget;
    // Deny only: one line the model sees in the tool result.
    std::string reason;
};

// Classify one shell command line -- what the model put in exec_shell.
Verdict evaluate(const std::string& command);

// Classify one already-split segment. Exposed for tests.
Verdict evaluateSegment(const std::string& segment);

// Split on ; && || | and newlines, but never inside single or double quotes:
// `git commit -m "fix & feature"` must stay one segment. The CodeWhale lesson
// is in here: their early version missed `|`, so `echo x | rm -rf $HOME`
// checked only the `echo`.
std::vector<std::string> splitSegments(const std::string& command);

// First real command token of a segment, after peeling `cmd /c`, `sh -c`,
// `powershell -c`, `sudo`, `env`, leading VAR=value assignments and any
// directory prefix (`/bin/rm` -> `rm`). Empty when nothing is left.
std::string firstCommandToken(const std::string& segment);

} // namespace delete_guard
