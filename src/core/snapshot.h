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

// Open the store for `workspace` and take the baseline on first use.
//
// The baseline is the expensive part: it reads and hashes every tracked file
// once, because "undo the first edit" is impossible without the content that
// existed before it. After that, a turn costs two stat-only scans.
//
// keepDays prunes other workspaces' stores; maxWorkspaceGb <= 0 disables the
// capacity gate. Returns false and fills err when snapshots cannot run here
// (read-only store, workspace over the gate) -- the caller keeps working.
bool open(const std::string& workspace, int keepDays, int maxWorkspaceGb, std::string& err);

bool isAvailable();
std::string unavailableReason();

int beginTurn(const std::string& sessionId, std::string& err); // seq, or -1
bool endTurn(int turnSeq, std::string& err);

// Highest turn of this session that finished and has not been undone, or -1.
int lastTurn(const std::string& sessionId);

Report undoLast(const std::string& sessionId, bool force);

// Roll the workspace back to the state before `turnSeq`; later turns are
// undone too (in reverse order).
Report restoreTo(const std::string& sessionId, int turnSeq, bool force);

// Test seam: forget every cached path and close the database.
void close();

} // namespace snapshot
