// proJV -- Linux process runner (fork/exec + pipe + killpg)
#include "platform/iprocess_runner.h"

#ifndef _WIN32
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <signal.h>
#include <fcntl.h>
#include <cstring>
#include <thread>
#include <mutex>
#include <chrono>
#include <cstdlib>
#include <cerrno>

static constexpr int kPollMs = 100;
static constexpr int kIdleMs = 30000;

class ProcessRunnerLinux : public IProcessRunner {
public:
    ProcessResult Run(const ProcessConfig& cfg) override {
        ProcessResult result;
        std::lock_guard<std::mutex> lock(mutex_);

        // Build command: block git interactive prompts, cd to workDir
        std::string fullCmd =
            "export GIT_TERMINAL_PROMPT=0; "
            "export GCM_INTERACTIVE=Never; "
            "export GIT_ASKPASS=; "
            "export SSH_ASKPASS=; "
            "export CORE_ASKPASS=; ";
        if (!cfg.workDir.empty())
            fullCmd += "cd \"" + cfg.workDir + "\" && ";
        fullCmd += cfg.command;

        // Create pipes (stdin, stdout, stderr)
        int inPipe[2] = {-1, -1};
        int outPipe[2] = {-1, -1};
        int errPipe[2] = {-1, -1};

        if (pipe(outPipe) < 0 || pipe(errPipe) < 0) {
            result.stdout_ = "Error: pipe() failed";
            return result;
        }
        if (!cfg.stdinContent.empty() && pipe(inPipe) < 0) {
            close(outPipe[0]); close(outPipe[1]);
            close(errPipe[0]); close(errPipe[1]);
            result.stdout_ = "Error: stdin pipe() failed";
            return result;
        }

        pid_t pid = fork();
        if (pid < 0) {
            result.stdout_ = "Error: fork() failed";
            return result;
        }

        if (pid == 0) {
            // === CHILD ===
            setpgid(0, 0);  // new process group for killpg

            dup2(outPipe[1], STDOUT_FILENO);
            dup2(errPipe[1], STDERR_FILENO);
            if (!cfg.stdinContent.empty()) {
                dup2(inPipe[0], STDIN_FILENO);
            } else {
                int devnull = open("/dev/null", O_RDONLY);
                if (devnull >= 0) { dup2(devnull, STDIN_FILENO); close(devnull); }
            }

            // Close all pipe fds (child only needs stdio dupes)
            close(outPipe[0]); close(outPipe[1]);
            close(errPipe[0]); close(errPipe[1]);
            if (!cfg.stdinContent.empty()) { close(inPipe[0]); close(inPipe[1]); }

            if (!cfg.workDir.empty()) chdir(cfg.workDir.c_str());

            for (auto& [k, v] : cfg.extraEnv)
                setenv(k.c_str(), v.c_str(), 1);

            execl("/bin/sh", "sh", "-c", fullCmd.c_str(), (char*)nullptr);
            _exit(127);  // execl failed
        }

        // === PARENT ===
        close(outPipe[1]);
        close(errPipe[1]);
        if (!cfg.stdinContent.empty()) close(inPipe[0]);

        // Write stdin
        if (!cfg.stdinContent.empty()) {
            ssize_t n = write(inPipe[1], cfg.stdinContent.data(),
                              cfg.stdinContent.size());
            (void)n;
            close(inPipe[1]);
        }

        pid_ = pid;
        cancelled_ = false;

        // Reader threads
        std::string out, err;
        std::mutex outMtx;
        std::atomic<bool> readerDone{false};

        std::thread reader([&]() {
            char buf[4096];
            ssize_t n;
            while ((n = read(outPipe[0], buf, sizeof(buf) - 1)) > 0) {
                buf[n] = '\0';
                std::lock_guard<std::mutex> lk(outMtx);
                out += buf;
                err += buf;  // combine stderr into output
            }
            while ((n = read(errPipe[0], buf, sizeof(buf) - 1)) > 0) {
                buf[n] = '\0';
                std::lock_guard<std::mutex> lk(outMtx);
                err += buf;
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
            int status;
            pid_t w = waitpid(pid, &status, WNOHANG);
            if (w > 0) {
                if (WIFEXITED(status))
                    result.exitCode = WEXITSTATUS(status);
                else if (WIFSIGNALED(status))
                    result.exitCode = 128 + WTERMSIG(status);
                break;
            }

            if (cancelled_) {
                killpg(pid, SIGTERM);
                // Wait up to 2s for graceful shutdown
                for (int i = 0; i < 20; i++) {
                    usleep(100000);
                    if (waitpid(pid, &status, WNOHANG) > 0) break;
                }
                if (waitpid(pid, &status, WNOHANG) == 0)
                    killpg(pid, SIGKILL);
                waitpid(pid, &status, 0);
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
            if (now > deadline || (now - lastOut) > std::chrono::milliseconds(kIdleMs)) {
                killpg(pid, SIGKILL);
                waitpid(pid, &status, 0);
                killed = true;
                result.timedOut = true;
                break;
            }

            usleep(kPollMs * 1000);
        }

        if (killed) { close(outPipe[0]); close(errPipe[0]); }
        reader.join();
        if (!killed) { close(outPipe[0]); close(errPipe[0]); }
        pid_ = 0;

        result.stdout_ = out + err;
        while (!result.stdout_.empty()
               && (result.stdout_.back() == '\n' || result.stdout_.back() == '\r'))
            result.stdout_.pop_back();
        if (result.stdout_.size() > 32000)
            result.stdout_ = result.stdout_.substr(0, 32000) + "\n... (truncated)";
        return result;
    }

    void Cancel() override { cancelled_ = true; }

    bool IsRunning() const override { return pid_ != 0; }

private:
    std::mutex mutex_;
    std::atomic<bool> cancelled_{false};
    pid_t pid_ = 0;
};

IProcessRunner* CreateProcessRunner() { return new ProcessRunnerLinux(); }
#endif
