// proJV -- Linux system utilities
#include "platform/isystem_util.h"

#ifndef _WIN32
#include <unistd.h>
#include <signal.h>
#include <execinfo.h>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
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

    std::string GetUserConfigDir() override {
        return GetExeDir() + "/projv_files";
    }

    std::string GetSystemFontPath(const std::string& fontName) override {
        // 1. Try bundled font
        std::string bundled = GetExeDir() + "/assets/" + fontName;
        if (fs::exists(bundled)) return bundled;

        // 2. Try common system CJK font paths
        static const char* paths[] = {
            "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
            "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
            "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
            "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
            "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
            "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
            nullptr
        };
        for (const char** p = paths; *p; ++p) {
            if (fs::exists(*p)) return *p;
        }
        return "";
    }

    void InstallCrashHandler() override {
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

    std::string GetPathSeparator() override {
        return "/";
    }

    void OpenUrl(const std::string& url) override {
        pid_t pid = fork();
        if (pid == 0) {
            execlp("xdg-open", "xdg-open", url.c_str(), (char*)nullptr);
            _exit(1);
        }
    }

private:
    static SystemUtilLinux* prevHandler_;
    static std::mutex       crashMutex_;
    std::function<void()>   exitCallback_;

    static void ExitAction(int sig) {
        (void)sig;
        if (prevHandler_ && prevHandler_->exitCallback_) {
            prevHandler_->exitCallback_();
        }
        _exit(0);
    }

    static void CrashAction(int sig, siginfo_t*, void*) {
        std::lock_guard<std::mutex> lock(crashMutex_);

        std::string exeDir = prevHandler_ ? prevHandler_->GetExeDir() : ".";
        auto logPath = fs::path(exeDir) / "proJV_crash.log";

        std::ofstream log(logPath, std::ios::app);
        if (log.is_open()) {
            auto t = std::time(nullptr);
            char tb[64]; struct tm local;
            localtime_r(&t, &local);
            strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M:%S", &local);

            log << "=== proJV CRASH " << tb << " ===" << std::endl;
            log << "Signal: " << sig << std::endl;

            void* stack[64];
            int n = backtrace(stack, 64);
            char** syms = backtrace_symbols(stack, n);
            if (syms) {
                for (int i = 0; i < n; i++)
                    log << "  " << syms[i] << std::endl;
                free(syms);
            }
            log << std::endl;
            log.close();
        }
        _exit(1);
    }
};

SystemUtilLinux* SystemUtilLinux::prevHandler_ = nullptr;
std::mutex       SystemUtilLinux::crashMutex_;

ISystemUtil* CreateSystemUtil() { return new SystemUtilLinux(); }
#endif
