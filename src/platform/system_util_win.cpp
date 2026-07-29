// proJV -- Windows system utilities
#include "platform/isystem_util.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <dbghelp.h>
#include <filesystem>
#include <fstream>
#include <ctime>
#include <mutex>

namespace fs = std::filesystem;

class SystemUtilWin : public ISystemUtil {
public:
    std::string GetExeDir() override {
        char buf[MAX_PATH];
        GetModuleFileNameA(nullptr, buf, MAX_PATH);
        return fs::path(buf).parent_path().string();
    }

    std::string GetUserConfigDir() override {
        return GetExeDir() + "\\projv_files";
    }

    std::string GetSystemFontPath(const std::string& fontName) override {
        // 1. Try bundled font
        std::string bundled = GetExeDir() + "\\assets\\" + fontName;
        if (fs::exists(bundled)) return bundled;

        // 2. Try C:/Windows/Fonts/
        std::string sysPath = "C:/Windows/Fonts/" + fontName;
        if (fs::exists(sysPath)) return sysPath;

        return "";
    }

    void InstallCrashHandler() override {
        GetModuleFileNameA(nullptr, crashExePath_, MAX_PATH);
        SetUnhandledExceptionFilter(UnhandledHandler);
        prevHandler_ = this;
    }

    std::string GetPathSeparator() override {
        return "\\";
    }

    void OpenUrl(const std::string& url) override {
        ShellExecuteA(nullptr, "open", url.c_str(),
                      nullptr, nullptr, SW_SHOWNORMAL);
    }

private:
    static SystemUtilWin* prevHandler_;
    static char           crashExePath_[MAX_PATH];
    static std::mutex     crashMutex_;

    static LONG WINAPI UnhandledHandler(EXCEPTION_POINTERS* pExp) {
        std::lock_guard<std::mutex> lock(crashMutex_);

        fs::path exeDir = fs::path(crashExePath_).parent_path();
        auto logPath = exeDir / "proJV_crash.log";

        std::ofstream log(logPath, std::ios::app);
        if (log.is_open()) {
            auto t = std::time(nullptr);
            char tb[64]; struct tm local;
            localtime_s(&local, &t);
            strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M:%S", &local);

            DWORD code = pExp->ExceptionRecord->ExceptionCode;
            PVOID addr = pExp->ExceptionRecord->ExceptionAddress;
            log << "=== proJV CRASH " << tb << " ===" << std::endl;
            log << "Code: 0x" << std::hex << code << std::dec
                << "  Addr: 0x" << std::hex << (uintptr_t)addr << std::dec
                << std::endl;
            log.close();
        }

        // Minidump
        auto dumpPath = exeDir / "proJV_crash.dmp";
        HANDLE hDump = CreateFileW(dumpPath.wstring().c_str(),
            GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hDump != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei;
            mei.ThreadId = GetCurrentThreadId();
            mei.ExceptionPointers = pExp;
            mei.ClientPointers = FALSE;
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                hDump, MiniDumpNormal, &mei, nullptr, nullptr);
            CloseHandle(hDump);
        }

        return EXCEPTION_EXECUTE_HANDLER;
    }
};

SystemUtilWin* SystemUtilWin::prevHandler_ = nullptr;
char           SystemUtilWin::crashExePath_[MAX_PATH] = {};
std::mutex     SystemUtilWin::crashMutex_;

ISystemUtil* CreateSystemUtil() { return new SystemUtilWin(); }

#else
ISystemUtil* CreateSystemUtil() { return nullptr; }
#endif
