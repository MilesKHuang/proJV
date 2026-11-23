// proJV -- ISystemStats, Windows half (see isystem_stats.h).
//
// CPU and memory of a session come from the processes it owns: Toolhelp32 builds
// the parent links, GetProcessTimes gives cumulative CPU seconds, GetProcessMemoryInfo
// the resident set. CPU is a DIFFERENCE between two cumulative readings -- a fresh
// handle per tick reports the same total twice and would read as 0% forever.
//
// GPU comes from the counters Task Manager reads, through PDH:
// `\GPU Engine(*)\Utilization Percentage` and `\GPU Process Memory(*)\Dedicated
// Usage`, whose instance names carry `pid_<n>`. They are added as "English"
// counters (PdhAddEnglishCounterW) because the localized names differ per Windows
// language, and a Chinese Windows would otherwise find nothing.
//
// A counter set that does not exist is reported per group with `reason`, never as 0.
#include "isystem_stats.h"

#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

long long monotonicMs() {
    using namespace std::chrono;
    return static_cast<long long>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

double filetimeSeconds(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return static_cast<double>(u.QuadPart) / 1e7; // 100 ns units
}

// `roots` plus every descendant. A session's background task may itself be a
// shell that started an interpreter; the cost belongs to the session either way.
std::vector<DWORD> withDescendants(const std::vector<int>& roots) {
    std::vector<DWORD> seeds;
    for (int pid : roots) {
        if (pid > 0) seeds.push_back(static_cast<DWORD>(pid));
    }
    if (seeds.empty()) return seeds;
    std::vector<std::pair<DWORD, DWORD>> links;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof pe;
        if (Process32FirstW(snap, &pe)) {
            do {
                links.emplace_back(static_cast<DWORD>(pe.th32ProcessID),
                                   static_cast<DWORD>(pe.th32ParentProcessID));
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }
    std::vector<DWORD> out = seeds;
    bool grew = true;
    while (grew) {
        grew = false;
        for (const auto& link : links) {
            if (std::find(out.begin(), out.end(), link.first) != out.end()) continue;
            if (std::find(out.begin(), out.end(), link.second) != out.end()) {
                out.push_back(link.first);
                grew = true;
            }
        }
    }
    return out;
}

struct ProcReading {
    bool ok = false;
    double cpuSeconds = 0.0;
    unsigned long long rssBytes = 0;
};

ProcReading readProcess(DWORD pid) {
    ProcReading r;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!h) return r;
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(h, &created, &exited, &kernel, &user)) {
        r.cpuSeconds = filetimeSeconds(kernel) + filetimeSeconds(user);
        PROCESS_MEMORY_COUNTERS_EX mc{};
        mc.cb = sizeof mc;
        if (GetProcessMemoryInfo(h, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&mc), sizeof mc))
            r.rssBytes = static_cast<unsigned long long>(mc.WorkingSetSize);
        r.ok = true;
    }
    CloseHandle(h);
    return r;
}

// ---- GPU counters --------------------------------------------------------

struct GpuCounters {
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER util = nullptr;
    PDH_HCOUNTER mem = nullptr;
    bool open = false;
    bool failed = false;
    std::string reason;
};

// One counter instance, with its name COPIED OUT of the PDH buffer: the array the
// API fills points every szName into a buffer that dies with the call, so keeping
// those pointers (the first version did) is a use-after-free waiting for a GPU
// sample -- which is exactly what crashed the window on start-up.
struct CounterItem {
    std::wstring name;
    double value = 0.0;
};

std::vector<CounterItem> readArray(PDH_HCOUNTER counter) {
    std::vector<CounterItem> items;
    if (!counter) return items;
    DWORD size = 0;
    DWORD count = 0;
    PDH_STATUS st = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &size, &count, nullptr);
    if (st != PDH_MORE_DATA || size == 0) return items;
    std::vector<unsigned char> buf(size);
    auto* array = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
    st = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &size, &count, array);
    if (st != ERROR_SUCCESS) return items;
    items.reserve(count);
    for (DWORD i = 0; i < count; ++i) {
        CounterItem item;
        if (array[i].szName) item.name = array[i].szName; // copy, do not borrow
        if (array[i].FmtValue.CStatus == 0) item.value = array[i].FmtValue.doubleValue;
        items.push_back(std::move(item));
    }
    return items;
}

// `pid_1234_eng_0_engtype_3D` -> 1234.
DWORD instancePid(const wchar_t* name) {
    if (!name) return 0;
    unsigned long pid = 0;
    if (swscanf_s(name, L"pid_%lu_", &pid) != 1) return 0;
    return static_cast<DWORD>(pid);
}

bool openGpu(GpuCounters& g) {
    if (g.open || g.failed) return g.open;
    if (PdhOpenQueryW(nullptr, 0, &g.query) != ERROR_SUCCESS) {
        g.failed = true;
        g.reason = "PDH: cannot open a query";
        return false;
    }
    if (PdhAddEnglishCounterW(g.query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &g.util) !=
        ERROR_SUCCESS) {
        PdhCloseQuery(g.query);
        g.query = nullptr;
        g.failed = true;
        g.reason = "PDH: no GPU Engine utilization counter on this machine";
        return false;
    }
    if (PdhAddEnglishCounterW(g.query, L"\\GPU Process Memory(*)\\Dedicated Usage", 0, &g.mem) !=
        ERROR_SUCCESS) {
        g.mem = nullptr; // memory is a bonus; utilization is what the gauge needs
    }
    PdhCollectQueryData(g.query); // rate counters need two collections
    g.open = true;
    g.reason.clear();
    return true;
}

// One GPU sample for every tracked group. `pids` is the group's expanded pid list.
std::map<int, GpuSample> sampleGpuFor(const std::vector<TrackedGroup>& groups, GpuCounters& g) {
    std::map<int, GpuSample> out;
    if (!openGpu(g)) {
        for (const auto& group : groups) {
            GpuSample s;
            s.reason = g.reason;
            out[group.key] = s;
        }
        return out;
    }
    if (PdhCollectQueryData(g.query) != ERROR_SUCCESS) {
        for (const auto& group : groups) {
            GpuSample s;
            s.reason = "PDH: collection failed";
            out[group.key] = s;
        }
        return out;
    }
    const auto utilItems = readArray(g.util);
    const auto memItems = readArray(g.mem);
    std::map<DWORD, double> utilPerPid;
    for (const auto& item : utilItems) {
        const DWORD pid = instancePid(item.name.c_str());
        if (!pid) continue;
        utilPerPid[pid] += item.value; // engines sum (3D + Copy + Video)
    }
    std::map<DWORD, unsigned long long> memPerPid;
    for (const auto& item : memItems) {
        const DWORD pid = instancePid(item.name.c_str());
        if (!pid) continue;
        const double bytes = item.value;
        // `Dedicated Usage` occasionally reports a value no adapter could hold;
        // beyond 64 GB it is junk, and printing it is worse than printing "-".
        if (!(bytes > 0.0) || bytes > 64.0 * 1024.0 * 1024.0 * 1024.0) continue;
        const auto value = static_cast<unsigned long long>(bytes);
        auto& slot = memPerPid[pid];
        if (value > slot) slot = value;
    }
    for (const auto& group : groups) {
        GpuSample s;
        const std::vector<DWORD> pids = withDescendants(group.pids);
        // The counter exists, so a value is knowable: summing the group's engine
        // instances and finding none means this session is not using the GPU.
        s.valid = true;
        double util = 0.0;
        unsigned long long mem = 0;
        for (DWORD pid : pids) {
            const auto uit = utilPerPid.find(pid);
            if (uit != utilPerPid.end()) util += uit->second;
            const auto mit = memPerPid.find(pid);
            if (mit != memPerPid.end()) mem += mit->second;
        }
        s.utilPercent = std::min(100.0, util);
        s.dedicatedBytes = mem;
        out[group.key] = s;
    }
    return out;
}

class SystemStatsWin final : public ISystemStats {
  public:
    ~SystemStatsWin() override { stop(); }

    void start(int procIntervalMs, int gpuIntervalMs) override {
        std::lock_guard<std::mutex> lk(mutex_);
        if (running_) return;
        procMs_ = procIntervalMs > 100 ? procIntervalMs : 100;
        gpuMs_ = gpuIntervalMs > 200 ? gpuIntervalMs : 200;
        running_ = true;
        thread_ = std::thread([this] { loop(); });
    }

    void stop() override {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (!running_) return;
            running_ = false;
        }
        wake_.notify_all();
        if (thread_.joinable()) thread_.join(); // joined, never detached
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (gpu_.query) {
                PdhCloseQuery(gpu_.query);
                gpu_.query = nullptr;
                gpu_.open = false;
            }
        }
    }

    void setTracked(const std::vector<TrackedGroup>& groups) override {
        std::lock_guard<std::mutex> lk(mutex_);
        tracked_ = groups;
        // A pane that closed keeps no baseline: dropping the entry is what stops a
        // reused key from inheriting another session's CPU history.
        for (auto it = prev_.begin(); it != prev_.end();) {
            const bool alive = std::any_of(tracked_.begin(), tracked_.end(),
                                           [&](const TrackedGroup& g) { return g.key == it->first; });
            it = alive ? std::next(it) : prev_.erase(it);
        }
    }

    StatsSnapshot snapshot() const override {
        std::lock_guard<std::mutex> lk(mutex_);
        StatsSnapshot out;
        out.atMonotonicMs = atMs_;
        for (const auto& group : tracked_) {
            out.keys.push_back(group.key);
            const auto it = samples_.find(group.key);
            out.groups.push_back(it == samples_.end() ? GroupSample{} : it->second);
        }
        return out;
    }

  private:
    void loop() {
        auto lastProc = std::chrono::steady_clock::now();
        auto lastGpu = lastProc - std::chrono::seconds(3600); // force a first sample
        while (true) {
            std::unique_lock<std::mutex> lk(mutex_);
            if (wake_.wait_for(lk, std::chrono::milliseconds(200), [this] { return !running_; }))
                return;
            const auto now = std::chrono::steady_clock::now();
            const bool doProc =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - lastProc).count() >=
                procMs_;
            const bool doGpu =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - lastGpu).count() >=
                gpuMs_;
            std::vector<TrackedGroup> groups = tracked_;
            lk.unlock();
            if (doProc) {
                sampleProcs(groups);
                lastProc = now;
            }
            if (doGpu) {
                sampleGpuOnce(groups);
                lastGpu = now;
            }
        }
    }

    void sampleProcs(const std::vector<TrackedGroup>& groups) {
        SYSTEM_INFO si{};
        GetNativeSystemInfo(&si);
        int cores = static_cast<int>(si.dwNumberOfProcessors);
#if defined(ALL_PROCESSOR_GROUPS)
        const DWORD logical = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        if (logical > 0) cores = static_cast<int>(logical);
#endif
        const long long now = monotonicMs();
        std::lock_guard<std::mutex> lk(mutex_);
        for (const auto& group : groups) {
            const std::vector<DWORD> pids = withDescendants(group.pids);
            double seconds = 0.0;
            unsigned long long rss = 0;
            bool any = false;
            for (DWORD pid : pids) {
                const ProcReading r = readProcess(pid);
                if (!r.ok) continue;
                seconds += r.cpuSeconds;
                rss += r.rssBytes;
                any = true;
            }
            GroupSample sample = samples_[group.key];
            // A session with no child process is idle: valid zeros, not "unknown".
            sample.cpuValid = any;
            sample.cpuPercent = 0.0;
            sample.memValid = true;
            sample.rssBytes = rss;
            if (any) {
                const auto it = prev_.find(group.key);
                if (it != prev_.end() && now > it->second.second) {
                    const double elapsed = static_cast<double>(now - it->second.second) / 1000.0;
                    sample.cpuPercent = cpuPercentBetween(it->second.first, seconds, elapsed, cores);
                }
                prev_[group.key] = {seconds, now};
            } else {
                prev_.erase(group.key);
            }
            samples_[group.key] = sample;
        }
        atMs_ = now;
    }

    void sampleGpuOnce(const std::vector<TrackedGroup>& groups) {
        // PDH collection happens without the lock: it takes a few milliseconds and
        // `snapshot()` must never wait on it. `gpu_` is touched only by this thread
        // while running, and stop() joins before closing the query.
        std::map<int, GpuSample> gpu = sampleGpuFor(groups, gpu_);
        std::lock_guard<std::mutex> lk(mutex_);
        for (const auto& entry : gpu) samples_[entry.first].gpu = entry.second;
    }

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread thread_;
    bool running_ = false;
    int procMs_ = 1000;
    int gpuMs_ = 3000;
    std::vector<TrackedGroup> tracked_;
    std::map<int, GroupSample> samples_;
    std::map<int, std::pair<double, long long>> prev_; // key -> (cpu seconds, ms)
    long long atMs_ = 0;
    GpuCounters gpu_;
};

} // namespace

ISystemStats* CreateSystemStats() { return new SystemStatsWin(); }
