// proJV -- Windows process runner (CreateProcess + Job Object)
#include "platform/iprocess_runner.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <thread>
#include <mutex>
#include <chrono>
#include <vector>
#include <format>
#include <cstring>

static constexpr int kPollMs    = 100;
static constexpr int kIdleMs    = 30000;

class ProcessRunnerWin : public IProcessRunner {
public:
    ProcessResult Run(const ProcessConfig& cfg) override {
        ProcessResult result;
        std::lock_guard<std::mutex> lock(mutex_);

        // Build command with git-env blockers and cd
        std::string gitEnv =
            "set GIT_TERMINAL_PROMPT=0 && "
            "set GCM_INTERACTIVE=Never && "
            "set GIT_ASKPASS= && "
            "set SSH_ASKPASS= && "
            "set CORE_ASKPASS= && ";
        std::string fullCmd = gitEnv + cfg.command;
        if (!cfg.workDir.empty())
            fullCmd = "cd /d \"" + cfg.workDir + "\" && " + fullCmd;

        // Job Object (KILL_ON_JOB_CLOSE)
        HANDLE hJob = CreateJobObjectW(nullptr, nullptr);
        if (hJob) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
            jeli.BasicLimitInformation.LimitFlags =
                JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(hJob, JobObjectExtendedLimitInformation,
                                    &jeli, sizeof(jeli));
        }

        // Pipes
        HANDLE hOutRd = nullptr, hOutWr = nullptr;
        HANDLE hInRd = nullptr, hInWr = nullptr;
        SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};

        if (!CreatePipe(&hOutRd, &hOutWr, &sa, 0)) {
            if (hJob) CloseHandle(hJob);
            result.stdout_ = "Error: Failed to create stdout pipe";
            return result;
        }
        SetHandleInformation(hOutRd, HANDLE_FLAG_INHERIT, 0);

        if (!cfg.stdinContent.empty()) {
            if (!CreatePipe(&hInRd, &hInWr, &sa, 0)) {
                CloseHandle(hOutRd); CloseHandle(hOutWr);
                if (hJob) CloseHandle(hJob);
                result.stdout_ = "Error: Failed to create stdin pipe";
                return result;
            }
            SetHandleInformation(hInWr, HANDLE_FLAG_INHERIT, 0);
        }

        // Startup info
        STARTUPINFOW si = {sizeof(si)};
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = hOutWr;
        si.hStdError  = hOutWr;
        si.hStdInput  = cfg.stdinContent.empty() ? nullptr : hInRd;

        // Wide-char conversion
        int wlen = MultiByteToWideChar(CP_UTF8, 0, fullCmd.c_str(),
                                       (int)fullCmd.size(), nullptr, 0);
        std::wstring wcmd(wlen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, fullCmd.c_str(),
                            (int)fullCmd.size(), &wcmd[0], wlen);
        wcmd = L"cmd.exe /c " + wcmd;
        std::vector<wchar_t> cmdBuf(wcmd.begin(), wcmd.end());
        cmdBuf.push_back(L'\0');

        PROCESS_INFORMATION pi = {};
        BOOL ok = CreateProcessW(nullptr, cmdBuf.data(),
            nullptr, nullptr, TRUE,
            CREATE_SUSPENDED | CREATE_NO_WINDOW,
            nullptr, nullptr, &si, &pi);
        CloseHandle(hOutWr);
        if (!cfg.stdinContent.empty()) CloseHandle(hInRd);

        if (!ok) {
            CloseHandle(hOutRd);
            if (!cfg.stdinContent.empty()) CloseHandle(hInWr);
            if (hJob) CloseHandle(hJob);
            char buf[64];
            snprintf(buf, sizeof(buf),
                "Error: CreateProcess failed (%lu)", GetLastError());
            result.stdout_ = buf;
            return result;
        }

        if (hJob) AssignProcessToJobObject(hJob, pi.hProcess);
        ResumeThread(pi.hThread);

        // Write stdin
        if (!cfg.stdinContent.empty()) {
            DWORD written;
            WriteFile(hInWr, cfg.stdinContent.data(),
                      (DWORD)cfg.stdinContent.size(), &written, nullptr);
            CloseHandle(hInWr);
        }

        // Reader thread
        std::string out;
        std::mutex outMtx;
        std::atomic<bool> readerDone{false};
        hProc_ = pi.hProcess;
        cancelled_ = false;

        std::thread reader([&]() {
            char buf[4096];
            DWORD n;
            while (ReadFile(hOutRd, buf, sizeof(buf) - 1, &n, nullptr)
                   && n > 0) {
                buf[n] = '\0';
                std::lock_guard<std::mutex> lk(outMtx);
                out += buf;
            }
            readerDone.store(true);
        });

        // Wait loop
        bool killed = false;
        size_t lastSize = 0;
        auto lastOut = std::chrono::steady_clock::now();
        auto deadline = cfg.timeoutMs > 0
            ? std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(cfg.timeoutMs)
            : std::chrono::steady_clock::time_point::max();

        while (true) {
            DWORD wr = WaitForSingleObject(pi.hProcess, kPollMs);
            if (wr == WAIT_OBJECT_0) break;

            if (cancelled_) {
                if (hJob) TerminateJobObject(hJob, 1);
                else TerminateProcess(pi.hProcess, 1);
                killed = true;
                result.cancelled = true;
                break;
            }

            {
                std::lock_guard<std::mutex> lk(outMtx);
                if (out.size() > lastSize) {
                    lastSize = out.size();
                    lastOut = std::chrono::steady_clock::now();
                }
            }

            auto now = std::chrono::steady_clock::now();
            if (now > deadline) {
                if (hJob) TerminateJobObject(hJob, 1);
                else TerminateProcess(pi.hProcess, 1);
                killed = true;
                result.timedOut = true;
                break;
            }
            if (std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - lastOut).count() > kIdleMs) {
                if (hJob) TerminateJobObject(hJob, 1);
                else TerminateProcess(pi.hProcess, 1);
                killed = true;
                result.timedOut = true;
                break;
            }
            if (readerDone.load()) {
                DWORD w2 = WaitForSingleObject(pi.hProcess, 2000);
                if (w2 == WAIT_OBJECT_0) break;
                if (hJob) TerminateJobObject(hJob, 1);
                else TerminateProcess(pi.hProcess, 1);
                killed = true;
                break;
            }
        }

        if (killed) CloseHandle(hOutRd);
        reader.join();
        if (!killed) CloseHandle(hOutRd);
        CloseHandle(pi.hThread);

        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        if (hJob) CloseHandle(hJob);
        hProc_ = nullptr;

        result.stdout_ = std::move(out);
        result.exitCode = (int)code;

        while (!result.stdout_.empty()
               && (result.stdout_.back() == '\n' || result.stdout_.back() == '\r'))
            result.stdout_.pop_back();
        if (result.stdout_.size() > 32000)
            result.stdout_ = result.stdout_.substr(0, 32000) + "\n... (truncated)";
        return result;
    }

    void Cancel() override {
        cancelled_ = true;
    }

    bool IsRunning() const override {
        return hProc_ != nullptr;
    }

private:
    std::mutex mutex_;
    std::atomic<bool> cancelled_{false};
    HANDLE hProc_ = nullptr;
};

IProcessRunner* CreateProcessRunner() {
    return new ProcessRunnerWin();
}
#else
IProcessRunner* CreateProcessRunner() { return nullptr; }
#endif
