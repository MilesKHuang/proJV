// proJV -- MCP child process control, Windows implementation.
#include "core/mcp_process.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstring>

namespace mcp {
namespace {

std::wstring toWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::string lastErrorText(const char* what) {
    char buf[128];
    snprintf(buf, sizeof(buf), " (%lu)", GetLastError());
    return std::string(what) + buf;
}

// Classic Windows argv quoting: backslashes are literal, but a backslash run
// immediately before a quote is doubled and the quote is escaped.
std::wstring quoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
    std::wstring r = L"\"";
    for (auto it = a.begin();; ++it) {
        unsigned backslashes = 0;
        while (it != a.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == a.end()) {
            r.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            r.append(backslashes * 2 + 1, L'\\');
            r.push_back(L'"');
        } else {
            r.append(backslashes, L'\\');
            r.push_back(*it);
        }
    }
    r.push_back(L'"');
    return r;
}

void closeHandle(void*& h) {
    if (h) {
        CloseHandle(static_cast<HANDLE>(h));
        h = nullptr;
    }
}

// Inherited environment plus the configured overrides. Passing nullptr would
// inherit everything, which is correct only when there is nothing to add.
std::wstring buildEnvBlock(const std::map<std::string, std::string>& overrides) {
    std::vector<std::wstring> entries;
    LPWCH cur = GetEnvironmentStringsW();
    if (cur) {
        for (LPWCH p = cur; *p; p += wcslen(p) + 1) {
            std::wstring e(p);
            // "=C:=..." style entries are per-drive cwd markers; skip them.
            if (e.size() < 2 || e[0] == L'=') continue;
            size_t eq = e.find(L'=');
            if (eq == std::wstring::npos) continue;
            const std::string key = toUtf8(e.substr(0, eq));
            if (overrides.count(key)) continue;
            entries.push_back(e);
        }
        FreeEnvironmentStringsW(cur);
    }
    for (const auto& kv : overrides)
        entries.push_back(toWide(kv.first + "=" + kv.second));
    std::wstring block;
    for (auto& e : entries) {
        block += e;
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

// Resolve the executable the way the shell would, so a bad `command` produces
// "not found on PATH" instead of a bare CreateProcess error code.
bool resolveExe(const std::string& command, std::wstring& full) {
    const std::wstring w = toWide(command);
    if (w.empty()) return false;
    if (w.find_first_of(L"\\/") != std::wstring::npos) {
        if (GetFileAttributesW(w.c_str()) != INVALID_FILE_ATTRIBUTES) {
            full = w;
            return true;
        }
        return false;
    }
    wchar_t buf[MAX_PATH * 4];
    DWORD n = SearchPathW(nullptr, w.c_str(), L".exe", (DWORD)(sizeof(buf) / sizeof(buf[0])), buf,
                          nullptr);
    if (n == 0 || n >= sizeof(buf) / sizeof(buf[0])) return false;
    full.assign(buf, n);
    return true;
}

} // namespace

bool spawn(const SpawnSpec& spec, ProcHandles& h, std::string& err) {
    err.clear();
    h = ProcHandles{};

    std::wstring exe;
    if (!resolveExe(spec.command, exe)) {
        err = "command not found on PATH: " + spec.command;
        return false;
    }

    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE inRd = nullptr, inWr = nullptr;
    HANDLE outRd = nullptr, outWr = nullptr;
    HANDLE errRd = nullptr, errWr = nullptr;
    if (!CreatePipe(&inRd, &inWr, &sa, 0) || !CreatePipe(&outRd, &outWr, &sa, 0) ||
        !CreatePipe(&errRd, &errWr, &sa, 0)) {
        err = lastErrorText("CreatePipe failed");
        closeHandle((void*&)inRd);
        closeHandle((void*&)inWr);
        closeHandle((void*&)outRd);
        closeHandle((void*&)outWr);
        closeHandle((void*&)errRd);
        closeHandle((void*&)errWr);
        return false;
    }
    // Only the child's ends may be inherited.
    SetHandleInformation(inWr, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outRd, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errRd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inRd;
    si.hStdOutput = outWr;
    si.hStdError = errWr;

    std::wstring cmdline = quoteArg(exe);
    for (const auto& a : spec.args) {
        cmdline.push_back(L' ');
        cmdline += quoteArg(toWide(a));
    }
    std::vector<wchar_t> cmdBuf(cmdline.begin(), cmdline.end());
    cmdBuf.push_back(L'\0');

    std::wstring envBlock;
    LPVOID envPtr = nullptr;
    if (!spec.env.empty()) {
        envBlock = buildEnvBlock(spec.env);
        envPtr = (LPVOID)envBlock.data();
    }
    std::wstring wd;
    LPCWSTR wdPtr = nullptr;
    if (!spec.workDir.empty()) {
        wd = toWide(spec.workDir);
        wdPtr = wd.c_str();
    }

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
    }

    PROCESS_INFORMATION pi = {};
    const BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                                   CREATE_SUSPENDED | CREATE_NO_WINDOW, envPtr, wdPtr, &si, &pi);
    // The child owns its own ends now.
    closeHandle((void*&)inRd);
    closeHandle((void*&)outWr);
    closeHandle((void*&)errWr);

    if (!ok) {
        err = lastErrorText("CreateProcess failed");
        closeHandle((void*&)inWr);
        closeHandle((void*&)outRd);
        closeHandle((void*&)errRd);
        if (job) CloseHandle(job);
        return false;
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    h.hProc = pi.hProcess;
    h.hJob = job;
    h.hInWr = inWr;
    h.hOutRd = outRd;
    h.hErrRd = errRd;
    h.valid = true;
    return true;
}

bool writeLine(ProcHandles& h, const std::string& line, std::string& err) {
    if (!h.valid || !h.hInWr) {
        err = "stdin is closed";
        return false;
    }
    std::string data = line;
    data.push_back('\n');
    size_t off = 0;
    while (off < data.size()) {
        DWORD written = 0;
        if (!WriteFile(static_cast<HANDLE>(h.hInWr), data.data() + off, (DWORD)(data.size() - off),
                       &written, nullptr)) {
            err = lastErrorText("WriteFile(stdin) failed");
            return false;
        }
        off += written;
    }
    return true;
}

void closeStdin(ProcHandles& h) {
    closeHandle(h.hInWr);
}

size_t readAvailable(ProcHandles& h, std::string& out, std::string& errOut) {
    size_t total = 0;
    char buf[8192];
    auto drain = [&](void* handle, std::string& sink, bool& eofFlag) -> size_t {
        if (!handle || eofFlag) return 0;
        size_t got = 0;
        for (;;) {
            DWORD avail = 0;
            if (!PeekNamedPipe(static_cast<HANDLE>(handle), nullptr, 0, nullptr, &avail, nullptr)) {
                eofFlag = true; // broken pipe: the child is gone
                break;
            }
            if (avail == 0) break;
            DWORD want = avail < (DWORD)sizeof(buf) ? avail : (DWORD)sizeof(buf);
            DWORD n = 0;
            if (!ReadFile(static_cast<HANDLE>(handle), buf, want, &n, nullptr) || n == 0) {
                eofFlag = true;
                break;
            }
            sink.append(buf, n);
            got += n;
        }
        return got;
    };
    total += drain(h.hOutRd, out, h.eof);
    bool errEof = false;
    total += drain(h.hErrRd, errOut, errEof);
    return total;
}

bool alive(ProcHandles& h) {
    if (!h.valid || !h.hProc) return false;
    return WaitForSingleObject(static_cast<HANDLE>(h.hProc), 0) == WAIT_TIMEOUT;
}

void terminate(ProcHandles& h, bool force) {
    (void)force; // TerminateJobObject / TerminateProcess take no graceful step
    if (!h.valid) return;
    if (h.hJob) {
        TerminateJobObject(static_cast<HANDLE>(h.hJob), 1);
    } else if (h.hProc) {
        TerminateProcess(static_cast<HANDLE>(h.hProc), 1);
    }
}

void closeAll(ProcHandles& h) {
    closeHandle(h.hInWr);
    closeHandle(h.hOutRd);
    closeHandle(h.hErrRd);
    // Closing the job (KILL_ON_JOB_CLOSE) is what reaps any children the server
    // spawned of its own.
    closeHandle(h.hJob);
    closeHandle(h.hProc);
    h.valid = false;
}

} // namespace mcp

#else // !_WIN32 -- POSIX counterpart
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace mcp {

bool spawn(const SpawnSpec& spec, ProcHandles& h, std::string& err) {
    err.clear();
    h = ProcHandles{};
    int inPipe[2] = {-1, -1}, outPipe[2] = {-1, -1}, errPipe[2] = {-1, -1};
    if (pipe(inPipe) < 0 || pipe(outPipe) < 0 || pipe(errPipe) < 0) {
        err = "pipe() failed";
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
        err = "fork() failed";
        return false;
    }
    if (pid == 0) {
        // Child: own process group so terminate() can kill the whole tree.
        setpgid(0, 0);
        dup2(inPipe[0], STDIN_FILENO);
        dup2(outPipe[1], STDOUT_FILENO);
        dup2(errPipe[1], STDERR_FILENO);
        close(inPipe[0]);
        close(inPipe[1]);
        close(outPipe[0]);
        close(outPipe[1]);
        close(errPipe[0]);
        close(errPipe[1]);
        if (!spec.workDir.empty() && chdir(spec.workDir.c_str()) != 0) _exit(127);
        for (const auto& kv : spec.env)
            setenv(kv.first.c_str(), kv.second.c_str(), 1);
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(spec.command.c_str()));
        for (const auto& a : spec.args)
            argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(spec.command.c_str(), argv.data());
        _exit(127);
    }
    setpgid(pid, pid); // also from the parent, to close the race
    close(inPipe[0]);
    close(outPipe[1]);
    close(errPipe[1]);
    h.pid = (int)pid;
    h.fdIn = inPipe[1];
    h.fdOut = outPipe[0];
    h.fdErr = errPipe[0];
    h.valid = true;
    return true;
}

bool writeLine(ProcHandles& h, const std::string& line, std::string& err) {
    if (!h.valid || h.fdIn < 0) {
        err = "stdin is closed";
        return false;
    }
    std::string data = line;
    data.push_back('\n');
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = ::write(h.fdIn, data.data() + off, data.size() - off);
        if (n <= 0) {
            if (errno == EINTR) continue;
            err = std::string("write(stdin) failed: ") + strerror(errno);
            return false;
        }
        off += (size_t)n;
    }
    return true;
}

void closeStdin(ProcHandles& h) {
    if (h.fdIn >= 0) {
        close(h.fdIn);
        h.fdIn = -1;
    }
}

size_t readAvailable(ProcHandles& h, std::string& out, std::string& errOut) {
    size_t total = 0;
    char buf[8192];
    auto drain = [&](int fd, std::string& sink, bool& eofFlag) -> size_t {
        if (fd < 0 || eofFlag) return 0;
        size_t got = 0;
        for (;;) {
            struct pollfd p = {fd, POLLIN, 0};
            int r = poll(&p, 1, 0);
            if (r <= 0) break;
            if (p.revents & (POLLHUP | POLLERR)) {
                eofFlag = true;
                break;
            }
            ssize_t n = read(fd, buf, sizeof(buf));
            if (n <= 0) {
                eofFlag = true;
                break;
            }
            sink.append(buf, (size_t)n);
            got += (size_t)n;
            if ((size_t)n < sizeof(buf)) break;
        }
        return got;
    };
    total += drain(h.fdOut, out, h.eof);
    bool errEof = false;
    total += drain(h.fdErr, errOut, errEof);
    return total;
}

bool alive(ProcHandles& h) {
    if (!h.valid || h.pid <= 0) return false;
    int status = 0;
    return waitpid(h.pid, &status, WNOHANG) == 0;
}

void terminate(ProcHandles& h, bool force) {
    if (!h.valid || h.pid <= 0) return;
    killpg(h.pid, force ? SIGKILL : SIGTERM);
    if (!force) {
        for (int i = 0; i < 20; ++i) {
            int status = 0;
            if (waitpid(h.pid, &status, WNOHANG) == h.pid) return;
            usleep(100 * 1000);
        }
        killpg(h.pid, SIGKILL);
    }
}

void closeAll(ProcHandles& h) {
    if (h.fdIn >= 0) close(h.fdIn);
    if (h.fdOut >= 0) close(h.fdOut);
    if (h.fdErr >= 0) close(h.fdErr);
    h.fdIn = h.fdOut = h.fdErr = -1;
    h.valid = false;
}

} // namespace mcp

#endif // _WIN32
