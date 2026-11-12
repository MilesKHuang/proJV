// proJV -- Windows system utilities
#include "platform/isystem_util.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <dbghelp.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

class SystemUtilWin : public ISystemUtil {
  public:
    std::string GetExeDir() override {
        char buf[MAX_PATH];
        GetModuleFileNameA(nullptr, buf, MAX_PATH);
        return fs::path(buf).parent_path().string();
    }

    std::string GetUserConfigDir() override { return GetExeDir() + "\\projv_files"; }

    void InstallCrashHandler() override {
        // The crash callback must not create directories: it can run with a
        // corrupted heap or an exhausted stack. Prepare the target here, while
        // the process is still healthy.
        const std::string dir = GetUserConfigDir() + "\\logs";
        std::error_code ec;
        fs::create_directories(dir, ec);
        std::snprintf(crashDir_, sizeof(crashDir_), "%s", dir.c_str());
        SetUnhandledExceptionFilter(UnhandledHandler);
        prevHandler_ = this;
    }

    void InstallExitHandler(std::function<void()> onExit) override {
        exitCallback_ = std::move(onExit);
        prevHandler_ = this;
        SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
    }

    std::string GetPathSeparator() override { return "\\"; }

    void OpenUrl(const std::string& url) override {
        ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

  private:
    static SystemUtilWin* prevHandler_;
    static char crashDir_[MAX_PATH];
    static volatile LONG crashBusy_;
    std::function<void()> exitCallback_;

    static BOOL WINAPI ConsoleCtrlHandler(DWORD ctrl) {
        if (ctrl == CTRL_CLOSE_EVENT || ctrl == CTRL_C_EVENT || ctrl == CTRL_BREAK_EVENT) {
            if (prevHandler_ && prevHandler_->exitCallback_) {
                prevHandler_->exitCallback_();
            }
            ExitProcess(0);
            return TRUE;
        }
        return FALSE;
    }

    // Runs on the crashing thread. Nothing here may allocate, take a lock the
    // crashed thread already holds, or throw: no std::string, no ofstream, no
    // std::mutex, no filesystem. Win32 calls and stack buffers only.
    static LONG WINAPI UnhandledHandler(EXCEPTION_POINTERS* pExp) {
        if (InterlockedCompareExchange(&crashBusy_, 1, 0) != 0) return EXCEPTION_EXECUTE_HANDLER;

        SYSTEMTIME st;
        GetLocalTime(&st);
        char stamp[32];
        wsprintfA(stamp, "%04d%02d%02d-%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour,
                  st.wMinute, st.wSecond);

        char base[MAX_PATH];
        wsprintfA(base, "%s\\crash-%s", crashDir_, stamp);

        // 1) A one-line report a human can act on.
        char textPath[MAX_PATH];
        wsprintfA(textPath, "%s.log", base);
        HANDLE hText = CreateFileA(textPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hText != INVALID_HANDLE_VALUE) {
            const unsigned long long addr =
                (unsigned long long)(uintptr_t)pExp->ExceptionRecord->ExceptionAddress;
            char body[512];
            wsprintfA(body,
                      "=== proJV CRASH %04d-%02d-%02d %02d:%02d:%02d ===\r\n"
                      "code=0x%08lX address=0x%08lX%08lX\r\n",
                      st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                      (unsigned long)pExp->ExceptionRecord->ExceptionCode,
                      (unsigned long)(addr >> 32), (unsigned long)(addr & 0xFFFFFFFF));
            DWORD written = 0;
            WriteFile(hText, body, (DWORD)lstrlenA(body), &written, nullptr);
            CloseHandle(hText);
        }

        // 2) The minidump: the only artefact that carries a call stack.
        char dumpPath[MAX_PATH];
        wsprintfA(dumpPath, "%s.dmp", base);
        HANDLE hDump = CreateFileA(dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hDump != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei;
            mei.ThreadId = GetCurrentThreadId();
            mei.ExceptionPointers = pExp;
            mei.ClientPointers = FALSE;
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hDump, MiniDumpNormal,
                              &mei, nullptr, nullptr);
            CloseHandle(hDump);
        }

        return EXCEPTION_EXECUTE_HANDLER;
    }
};

SystemUtilWin* SystemUtilWin::prevHandler_ = nullptr;
char SystemUtilWin::crashDir_[MAX_PATH] = {};
volatile LONG SystemUtilWin::crashBusy_ = 0;

ISystemUtil* CreateSystemUtil() {
    return new SystemUtilWin();
}

#else
ISystemUtil* CreateSystemUtil() {
    return nullptr;
}
#endif
