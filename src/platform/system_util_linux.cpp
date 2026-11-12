// proJV -- Linux system utilities
#include "platform/isystem_util.h"

#ifndef _WIN32
#include <unistd.h>
#include <signal.h>
#include <execinfo.h>
#include <fcntl.h>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

class SystemUtilLinux : public ISystemUtil {
  public:
    std::string GetExeDir() override {
        char buf[4096] = {};
        ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            return fs::path(buf).parent_path().string();
        }
        return fs::current_path().string();
    }

    std::string GetUserConfigDir() override { return GetExeDir() + "/projv_files"; }

    void InstallCrashHandler() override {
        // Directories and paths are prepared here; a signal handler cannot
        // create them safely.
        const std::string dir = GetUserConfigDir() + "/logs";
        std::error_code ec;
        fs::create_directories(dir, ec);
        std::snprintf(crashDir_, sizeof(crashDir_), "%s", dir.c_str());

        struct sigaction sa;
        std::memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = CrashAction;
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGSEGV, &sa, nullptr);
        sigaction(SIGABRT, &sa, nullptr);
        prevHandler_ = this;
    }

    void InstallExitHandler(std::function<void()> onExit) override {
        exitCallback_ = std::move(onExit);
        prevHandler_ = this;
        struct sigaction sa;
        std::memset(&sa, 0, sizeof(sa));
        sa.sa_handler = ExitAction;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGHUP, &sa, nullptr);
        sigaction(SIGINT, &sa, nullptr);
        sigaction(SIGTERM, &sa, nullptr);
    }

    std::string GetPathSeparator() override { return "/"; }

    void OpenUrl(const std::string& url) override {
        pid_t pid = fork();
        if (pid == 0) {
            execlp("xdg-open", "xdg-open", url.c_str(), (char*)nullptr);
            _exit(1);
        }
    }

  private:
    static SystemUtilLinux* prevHandler_;
    static char crashDir_[4096];
    static volatile sig_atomic_t crashBusy_;
    std::function<void()> exitCallback_;

    static void ExitAction(int sig) {
        (void)sig;
        if (prevHandler_ && prevHandler_->exitCallback_) {
            prevHandler_->exitCallback_();
        }
        _exit(0);
    }

    // Signal context: no iostream, no std::string, no mutex, no allocation of
    // our own. backtrace_symbols() still mallocs, but without it the report has
    // no frames at all -- the trade is deliberate.
    static void CrashAction(int sig, siginfo_t*, void*) {
        if (crashBusy_) _exit(1);
        crashBusy_ = 1;

        const time_t t = std::time(nullptr);
        struct tm local;
        localtime_r(&t, &local);
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);

        char path[4096];
        std::snprintf(path, sizeof(path), "%s/crash-%s.log", crashDir_, stamp);
        const int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            char head[192];
            const int n = std::snprintf(head, sizeof(head), "=== proJV CRASH %s ===\nsignal: %d\n",
                                        stamp, sig);
            if (n > 0) {
                const ssize_t w = ::write(fd, head, (size_t)n);
                (void)w;
            }
            void* stack[64];
            const int frames = backtrace(stack, 64);
            char** syms = backtrace_symbols(stack, frames);
            if (syms) {
                for (int i = 0; i < frames; ++i) {
                    const ssize_t w1 = ::write(fd, syms[i], std::strlen(syms[i]));
                    const ssize_t w2 = ::write(fd, "\n", 1);
                    (void)w1;
                    (void)w2;
                }
                free(syms);
            }
            ::close(fd);
        }
        _exit(1);
    }
};

SystemUtilLinux* SystemUtilLinux::prevHandler_ = nullptr;
char SystemUtilLinux::crashDir_[4096] = {};
volatile sig_atomic_t SystemUtilLinux::crashBusy_ = 0;

ISystemUtil* CreateSystemUtil() {
    return new SystemUtilLinux();
}
#endif
