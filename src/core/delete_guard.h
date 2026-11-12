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

struct Verdict {
    Decision decision = Decision::Allow;
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
