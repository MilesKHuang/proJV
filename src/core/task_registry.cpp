// proJV -- background tasks (see task_registry.h for the design and the reasons).
#include "core/task_registry.h"

#include "tools/shell_output.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace bg_task {

namespace {
// Longest prefix of `s` that ends on a complete UTF-8 character and fits in
// maxBytes. Walking forward is the only way to be sure: dropping trailing
// continuation bytes is not enough, a lead byte can be the last one kept.
size_t utf8CompletePrefix(const std::string& s, size_t maxBytes) {
    size_t i = 0;
    while (i < s.size() && i < maxBytes) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = 1;
        if (c >= 0xF0)
            len = 4;
        else if (c >= 0xE0)
            len = 3;
        else if (c >= 0xC0)
            len = 2;
        if (i + len > maxBytes) break;
        i += len;
    }
    return i;
}
} // namespace

std::string latestLine(const std::string& s, size_t maxCols) {
    size_t end = s.size();
    while (end > 0 && (s[end - 1] == '\n' || s[end - 1] == '\r' || s[end - 1] == ' '))
        --end;
    if (end == 0) return std::string();
    size_t start = s.rfind('\n', end - 1);
    start = (start == std::string::npos) ? 0 : start + 1;
    while (start < end && (s[start] == '\r' || s[start] == ' ' || s[start] == '\t'))
        ++start;
    std::string line = s.substr(start, end - start);
    if (line.size() > maxCols) {
        line.resize(utf8CompletePrefix(line, maxCols));
        line += "...";
    }
    return line;
}

const char* stateLabel(State s) {
    switch (s) {
    case State::Running:
        return "running";
    case State::Done:
        return "done";
    case State::Failed:
        return "failed";
    case State::Killed:
        return "killed";
    case State::TimedOut:
        return "timed out";
    }
    return "?";
}

} // namespace bg_task

namespace {

long long nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

size_t fileBytes(const std::string& path) {
    std::error_code ec;
    const auto n = std::filesystem::file_size(path, ec);
    return ec ? 0 : static_cast<size_t>(n);
}

// Read at most the last `keep` bytes of the file. Never loads a 500 MB log.
std::string readTail(const std::string& path, size_t keep) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::string();
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size <= 0) return std::string();
    const std::streamoff start = size > static_cast<std::streamoff>(keep) ? size - keep : 0;
    in.seekg(start, std::ios::beg);
    std::string out(static_cast<size_t>(size - start), '\0');
    in.read(out.data(), static_cast<std::streamsize>(out.size()));
    out.resize(static_cast<size_t>(in.gcount()));
    return out;
}

} // namespace

struct TaskRegistry::Task {
    int id = 0;
    std::string command;
    std::string path;
    bg_task::State state = bg_task::State::Running;
    int exitCode = -1;
    long long startedAtMs = 0;
    bool killedByUser = false;
#ifdef _WIN32
    HANDLE job = nullptr;
    HANDLE proc = nullptr;
#else
    pid_t pid = 0;
#endif
};

TaskRegistry::TaskRegistry() {
    reaper_ = std::thread([this] { reaper(); });
}

TaskRegistry::~TaskRegistry() {
    shutdown();
}

std::shared_ptr<TaskRegistry::Task> TaskRegistry::find(int id) const {
    for (const auto& t : tasks_)
        if (t->id == id) return t;
    return nullptr;
}

int TaskRegistry::start(const std::string& command, const std::string& workDir,
                        std::string& error) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto task = std::make_shared<Task>();
    task->id = tasks_.empty() ? 1 : tasks_.back()->id + 1;
    task->command = command;
    task->startedAtMs = nowMs();
    std::error_code ec;
    const auto dir = std::filesystem::temp_directory_path(ec);
    // The pid is part of the name on purpose: without it a second proJV instance, or a
    // stale file left behind by a crashed one, makes CreateFileW fail with a sharing
    // violation -- and a background task then refuses to start for no visible reason.
#ifdef _WIN32
    const std::string owner = std::to_string(GetCurrentProcessId());
#else
    const std::string owner = std::to_string(::getpid());
#endif
    const std::string base = (ec ? std::filesystem::path(".") : dir).string() + "/projv_bg_" +
                             owner + "_" + std::to_string(task->id) + ".log";
    task->path = base;

#ifdef _WIN32
    std::string full = command;
    if (!workDir.empty()) full = "cd /d \"" + workDir + "\" && " + full;
    std::wstring wcmd(L"cmd.exe /c ");
    {
        const int n = MultiByteToWideChar(CP_UTF8, 0, full.c_str(), (int)full.size(), nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, full.c_str(), (int)full.size(), &w[0], n);
        wcmd += w;
    }

    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    const std::wstring wpath(base.begin(), base.end());
    HANDLE out = CreateFileW(wpath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) {
        error = "cannot create the output file";
        return 0;
    }

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
    }

    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = out;
    si.hStdError = out;
    si.hStdInput = nullptr;
    std::vector<wchar_t> cmdBuf(wcmd.begin(), wcmd.end());
    cmdBuf.push_back(L'\0');
    PROCESS_INFORMATION pi = {};
    const BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                                   CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(out);
    if (!ok) {
        if (job) CloseHandle(job);
        error = "CreateProcess failed (" + std::to_string(GetLastError()) + ")";
        return 0;
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    task->job = job;
    task->proc = pi.hProcess;
#else
    const std::string full = workDir.empty() ? command : ("cd \"" + workDir + "\" && " + command);
    const int fd = ::open(base.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        error = "cannot create the output file";
        return 0;
    }
    const pid_t pid = fork();
    if (pid == 0) {
        ::setpgid(0, 0); // own group, so killpg takes the whole tree
        ::dup2(fd, STDOUT_FILENO);
        ::dup2(fd, STDERR_FILENO);
        ::close(fd);
        ::execl("/bin/sh", "sh", "-c", full.c_str(), (char*)nullptr);
        _exit(127);
    }
    ::close(fd);
    if (pid < 0) {
        error = "fork failed";
        return 0;
    }
    task->pid = pid;
#endif

    tasks_.push_back(task);
    return task->id;
}

bool TaskRegistry::end(int id, std::string& error) {
    std::shared_ptr<Task> t;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        t = find(id);
        if (!t) {
            error = "no task with id " + std::to_string(id);
            return false;
        }
        if (t->state != bg_task::State::Running) {
            error = "task " + std::to_string(id) + " already finished (" +
                    bg_task::stateLabel(t->state) + ")";
            return false;
        }
        t->killedByUser = true;
        t->state = bg_task::State::Killed;
    }
#ifdef _WIN32
    if (t->job)
        TerminateJobObject(t->job, 1);
    else if (t->proc)
        TerminateProcess(t->proc, 1);
#else
    if (t->pid > 0) {
        killpg(t->pid, SIGTERM);
        killpg(t->pid, SIGKILL);
    }
#endif
    return true;
}

size_t TaskRegistry::endAll() {
    std::vector<int> ids;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        for (const auto& t : tasks_)
            if (t->state == bg_task::State::Running) ids.push_back(t->id);
    }
    size_t n = 0;
    for (int id : ids) {
        std::string err;
        if (end(id, err)) ++n;
    }
    return n;
}

std::string TaskRegistry::read(int id, int tailLines, bool fullOutput, std::string& error) const {
    std::shared_ptr<Task> t;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        t = find(id);
    }
    if (!t) {
        error = "no task with id " + std::to_string(id);
        return std::string();
    }
    const std::string raw = readTail(t->path, bg_task::kKeepBytes);
    return shell_output::apply(raw, tailLines, fullOutput).text;
}

std::vector<bg_task::Snapshot> TaskRegistry::snapshots() const {
    std::vector<bg_task::Snapshot> out;
    std::lock_guard<std::mutex> lk(mutex_);
    for (const auto& t : tasks_) {
        bg_task::Snapshot s;
        s.id = t->id;
        s.command = t->command;
        s.state = t->state;
        s.exitCode = t->exitCode;
        s.startedAtMs = t->startedAtMs;
        s.outputBytes = fileBytes(t->path);
        s.lastLine = bg_task::latestLine(readTail(t->path, 4096), 120);
        out.push_back(std::move(s));
    }
    return out;
}

bool TaskRegistry::empty() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return tasks_.empty();
}

size_t TaskRegistry::runningCount() const {
    std::lock_guard<std::mutex> lk(mutex_);
    size_t n = 0;
    for (const auto& t : tasks_)
        if (t->state == bg_task::State::Running) ++n;
    return n;
}

std::vector<int> TaskRegistry::takeFinished() {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<int> out = finished_;
    finished_.clear();
    return out;
}

void TaskRegistry::reaper() {
    while (!stopping_.load(std::memory_order_acquire)) {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            for (const auto& t : tasks_) {
                if (t->state != bg_task::State::Running) continue;
                if (nowMs() - t->startedAtMs > bg_task::kBackgroundTimeoutMs) {
#ifdef _WIN32
                    if (t->job)
                        TerminateJobObject(t->job, 1);
                    else if (t->proc)
                        TerminateProcess(t->proc, 1);
#else
                    killpg(t->pid, SIGKILL);
#endif
                    t->state = bg_task::State::TimedOut;
                    finished_.push_back(t->id);
                    continue;
                }
                bool exited = false;
                int code = 0;
#ifdef _WIN32
                if (t->proc && WaitForSingleObject(t->proc, 0) == WAIT_OBJECT_0) {
                    DWORD c = 0;
                    GetExitCodeProcess(t->proc, &c);
                    code = static_cast<int>(c);
                    exited = true;
                }
#else
                int status = 0;
                if (t->pid > 0 && waitpid(t->pid, &status, WNOHANG) == t->pid) {
                    code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                    exited = true;
                }
#endif
                if (exited) {
                    t->exitCode = code;
                    t->state = (code == 0) ? bg_task::State::Done : bg_task::State::Failed;
                    finished_.push_back(t->id);
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

void TaskRegistry::shutdown() {
    if (stopping_.exchange(true, std::memory_order_acq_rel)) return;
    if (reaper_.joinable()) reaper_.join();
    endAll();
    std::lock_guard<std::mutex> lk(mutex_);
    for (const auto& t : tasks_) {
#ifdef _WIN32
        if (t->proc) CloseHandle(t->proc);
        if (t->job) CloseHandle(t->job);
#endif
        std::error_code ec;
        std::filesystem::remove(t->path, ec);
    }
    tasks_.clear();
    finished_.clear();
}
