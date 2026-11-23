// proJV -- ISystemStats, Linux half (see isystem_stats.h).
//
// One /proc scan per tick builds pid -> (ppid, CPU seconds, RSS); each tracked
// group then sums the processes it owns plus their descendants. No child process,
// no python, no nvidia-smi.
//
// GPU utilization is the one number Linux does not offer per process: amdgpu owns a
// per-card `gpu_busy_percent`, so attributing it to one session would be a lie.
// Every group reports GPU as unknown with that reason and prints "-". CPU and
// memory, the two numbers a session can actually own here, are exact.
#include "isystem_stats.h"

#include <dirent.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
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

std::string readFile(const std::string& path) {
    std::ifstream in(path);
    if (!in) return std::string();
    std::string out;
    std::getline(in, out);
    return out;
}

bool numeric(const char* name) {
    if (!name || !*name) return false;
    for (const char* p = name; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
    }
    return true;
}

struct ProcInfo {
    int ppid = 0;
    double cpuSeconds = 0.0;
    unsigned long long rssBytes = 0;
    bool ok = false;
};

// Parsing starts after the LAST ')' because the command name may contain spaces.
ProcInfo readProc(int pid) {
    ProcInfo info;
    const std::string stat = readFile("/proc/" + std::to_string(pid) + "/stat");
    if (stat.empty()) return info;
    const size_t close = stat.rfind(')');
    if (close == std::string::npos) return info;
    const char* p = stat.c_str() + close + 1;
    while (*p == ' ') ++p;
    if (*p && (*p < '0' || *p > '9')) { // the state letter
        while (*p && *p != ' ') ++p;
    }
    long long fields[12] = {0};
    int count = 0;
    while (*p && count < 12) {
        while (*p == ' ') ++p;
        if (!*p) break;
        char* end = nullptr;
        fields[count] = std::strtoll(p, &end, 10);
        if (!end || end == p) break;
        p = end;
        ++count;
    }
    if (count < 12) return info;
    const long ticks = sysconf(_SC_CLK_TCK);
    if (ticks <= 0) return info;
    info.ppid = static_cast<int>(fields[0]);
    info.cpuSeconds = static_cast<double>(fields[10] + fields[11]) / static_cast<double>(ticks);
    const std::string statm = readFile("/proc/" + std::to_string(pid) + "/statm");
    if (!statm.empty()) {
        unsigned long long size = 0;
        unsigned long long resident = 0;
        if (std::sscanf(statm.c_str(), "%llu %llu", &size, &resident) == 2) {
            const long page = sysconf(_SC_PAGESIZE);
            if (page > 0) info.rssBytes = resident * static_cast<unsigned long long>(page);
        }
    }
    info.ok = true;
    return info;
}

std::map<int, ProcInfo> scanProcs() {
    std::map<int, ProcInfo> table;
    DIR* dir = opendir("/proc");
    if (!dir) return table;
    while (const dirent* entry = readdir(dir)) {
        if (!numeric(entry->d_name)) continue;
        const int pid = std::atoi(entry->d_name);
        ProcInfo info = readProc(pid);
        if (info.ok) table[pid] = info;
    }
    closedir(dir);
    return table;
}

std::vector<int> withDescendants(const std::vector<int>& roots,
                                 const std::map<int, ProcInfo>& table) {
    std::vector<int> out;
    for (int pid : roots) {
        if (pid > 0) out.push_back(pid);
    }
    bool grew = true;
    while (grew) {
        grew = false;
        for (const auto& entry : table) {
            if (std::find(out.begin(), out.end(), entry.first) != out.end()) continue;
            if (std::find(out.begin(), out.end(), entry.second.ppid) != out.end()) {
                out.push_back(entry.first);
                grew = true;
            }
        }
    }
    return out;
}

} // namespace

class SystemStatsLinux final : public ISystemStats {
  public:
    ~SystemStatsLinux() override { stop(); }

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
    }

    void setTracked(const std::vector<TrackedGroup>& groups) override {
        std::lock_guard<std::mutex> lk(mutex_);
        tracked_ = groups;
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
        auto lastGpu = lastProc - std::chrono::seconds(3600);
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
        long cores = sysconf(_SC_NPROCESSORS_ONLN);
        if (cores <= 0) cores = 1;
        const std::map<int, ProcInfo> table = scanProcs();
        const long long now = monotonicMs();
        std::lock_guard<std::mutex> lk(mutex_);
        for (const auto& group : groups) {
            const std::vector<int> pids = withDescendants(group.pids, table);
            double seconds = 0.0;
            unsigned long long rss = 0;
            bool any = false;
            for (int pid : pids) {
                const auto it = table.find(pid);
                if (it == table.end()) continue;
                seconds += it->second.cpuSeconds;
                rss += it->second.rssBytes;
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
                    sample.cpuPercent = cpuPercentBetween(it->second.first, seconds, elapsed,
                                                          static_cast<int>(cores));
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
        // No per-process GPU counters here (see the file comment): every session gets
        // "unknown" with the reason attached, never a shared card number.
        std::lock_guard<std::mutex> lk(mutex_);
        for (const auto& group : groups) {
            GpuSample s = samples_[group.key].gpu;
            s.valid = false;
            s.reason = "no per-process GPU counters on Linux";
            samples_[group.key].gpu = s;
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread thread_;
    bool running_ = false;
    int procMs_ = 1000;
    int gpuMs_ = 3000;
    std::vector<TrackedGroup> tracked_;
    std::map<int, GroupSample> samples_;
    std::map<int, std::pair<double, long long>> prev_;
    long long atMs_ = 0;
};

ISystemStats* CreateSystemStats() { return new SystemStatsLinux(); }
