// proJV -- resource sampling for the GUI's per-session gauges.
//
// The question this interface answers is NOT "how busy is this machine" (a user
// opens the OS panel for that) but "what does each session cost": one CPU figure,
// one memory figure and one GPU figure per session, drawn on that session's row.
//
// A session in proJV is a SessionRuntime inside ONE process, so the numbers are
// attributed by the processes that session owns:
//
//   - its background tasks            (TaskRegistry per runtime, pid exposed)
//   - its MCP servers                 (one Manager per runtime, handles exposed)
//   - the tool child, if any          (one runner for the whole process, so the
//                                      child belongs to the single busy session)
//
// The agent's own CPU cannot be split between sessions and is not reported. What
// also cannot be measured must stay UNKNOWN: `valid == false` prints "-", never a
// fabricated 0 -- "GPU 0%" and "GPU unknown" are different claims.
//
// Three rules kept from the single-tree version this replaces:
//   1. Sampling runs on its own thread, and stop() JOINS it. The UI only copies a
//      snapshot under a lock.
//   2. Nothing here spawns a process and nothing goes through IProcessRunner:
//      counters are read. Sampling that queued behind a 30-minute shell command
//      would be useless, and pooling the runner is forbidden (iprocess_runner.h).
//   3. CPU is a difference between two cumulative readings, percent of the whole
//      machine (cores in the denominator), which is how a task manager reports it.
#pragma once

#include <string>
#include <vector>

// One session's processes.
struct TrackedGroup {
    int key = 0;                 // the caller's id: the pane index in the studio
    std::vector<int> pids;       // that session's children (tasks, MCP servers, tool)
};

struct GpuSample {
    bool valid = false;
    double utilPercent = 0.0;
    unsigned long long dedicatedBytes = 0;
    std::string reason;
};

struct GroupSample {
    bool cpuValid = false; // false on the first sample of a group (it is a baseline)
    double cpuPercent = 0.0;
    unsigned long long rssBytes = 0;
    bool memValid = false; // false until a sample carries a readable RSS
    GpuSample gpu;
};

struct StatsSnapshot {
    std::vector<int> keys;               // same order as groups
    std::vector<GroupSample> groups;
    long long atMonotonicMs = 0;
};

// CPU percentage from two cumulative readings. Pure: `(1.0, 2.0, 1.0, 4) == 25`.
double cpuPercentBetween(double beforeSeconds, double nowSeconds, double elapsedSeconds, int cores);

class ISystemStats {
  public:
    virtual ~ISystemStats() = default;
    virtual void start(int procIntervalMs = 1000, int gpuIntervalMs = 3000) = 0;
    // Stop and JOIN the sampling thread, before the runtime's own cleanup.
    virtual void stop() = 0;
    // Replace what is tracked. Called every frame with the current panes; the
    // sampler keeps one baseline per key, so a pane keeping its key keeps its
    // percentage instead of restarting from "unknown".
    virtual void setTracked(const std::vector<TrackedGroup>& groups) = 0;
    virtual StatsSnapshot snapshot() const = 0;
};

ISystemStats* CreateSystemStats();
