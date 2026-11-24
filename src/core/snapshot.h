#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace snapshot {

// What a restore actually did. The caller turns this into one [Snapshot]
// message: the model has to be told that the files it edited are gone.
struct Report {
    bool ok = false;
    bool needsForce = false; // some target changed after the turn; repeat with force
    std::string error;
    int turnSeq = -1;
    std::vector<std::string> restored; // written back from a blob
    std::vector<std::string> removed;  // deleted (they were added by that turn)
    std::vector<std::string> skipped;  // torn read at scan time, left alone
};

// A handle to ONE workspace's store. Two panes on the same folder share it: a
// second open() of an already-open workspace returns the same handle, so they
// share one baseline, one blob directory and one index.db -- that is the
// "panels share snapshots" the studio needs. Two panes on different folders get
// different handles and cannot see each other's edits.
//
// The turns inside a store are per session: turns.session_id is the session
// database name, so /undo in one pane never selects another pane's turn, and a
// turn only records files that no other session produced (see the claims table).
using Store = int;
constexpr Store kNoStore = -1;

// Open the store for `workspace` and take the baseline on first use. Returns the
// handle for that workspace, or kNoStore when not even a key can be formed (empty
// workspace). A workspace that cannot be tracked (read-only store, over the
// capacity gate) still gets a handle whose isAvailable() is false and whose reason
// comes back in err -- the caller keeps working, and a second open() of the same
// folder does not pay for the scan again.
//
// The baseline is the expensive part: it reads and hashes every tracked file
// once, because "undo the first edit" is impossible without the content that
// existed before it. After that, a turn costs two stat-only scans.
//
// keepDays prunes other workspaces' stores; maxWorkspaceGb <= 0 disables the
// capacity gate.
Store open(const std::string& workspace, int keepDays, int maxWorkspaceGb, std::string& err);

bool isAvailable(Store s);

int beginTurn(Store s, const std::string& sessionId, std::string& err); // seq, or -1
bool endTurn(Store s, int turnSeq, std::string& err);

// Highest turn of this session that finished and has not been undone, or -1.
int lastTurn(Store s, const std::string& sessionId);

Report undoLast(Store s, const std::string& sessionId, bool force);

// Roll the workspace back to the state before `turnSeq`; later turns are
// undone too (in reverse order).
Report restoreTo(Store s, const std::string& sessionId, int turnSeq, bool force);

// Test seam: forget every cached path and close every store.
void close();

} // namespace snapshot
