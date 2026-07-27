// proJV - Minimal DeepSeek AI Agent
// Built with Dear ImGui + OpenGL 3.3 + GLFW  (S1: migrated from D3D11/Win32)
//
// S1 keeps the SEH crash handler here (will move to SystemUtilWin in S5).

#include "gui_backend.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

#include <imgui.h>
#include <loguru.hpp>

#include "ui/app.h"
#include "ui/theme.h"
#include "core/config.h"

// --- Windows-only dependencies for S1 (removed in S5) ----------------------
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <commdlg.h>
#endif

// --- Global state ----------------------------------------------------------
static App            g_app;
static GuiBackend     g_backend;

// --- Crash handler (Windows SEH) -- stays here until S5 --------------------

#ifdef _WIN32
static std::mutex crashMutex;
static char      crashExePath[MAX_PATH];

static LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS* pExp) {
    std::lock_guard<std::mutex> lock(crashMutex);

    // 1. Write crash details to log
    std::filesystem::path exeDir = std::filesystem::path(crashExePath).parent_path();
    auto crashLogPath = exeDir / "proJV_crash.log";
    std::ofstream crashLog(crashLogPath, std::ios::app);
    if (crashLog.is_open()) {
        auto t = std::time(nullptr);
        char timeBuf[64];
        struct tm local;
        localtime_s(&local, &t);
        strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &local);

        DWORD code = pExp->ExceptionRecord->ExceptionCode;
        PVOID addr = pExp->ExceptionRecord->ExceptionAddress;
        crashLog << "=== proJV CRASH " << timeBuf << " ===" << std::endl;
        crashLog << "Exception code: 0x" << std::hex << code << std::dec << std::endl;
        crashLog << "Exception addr: 0x" << std::hex << (uintptr_t)addr << std::dec << std::endl;

        const char* name = "UNKNOWN";
        switch (code) {
            case EXCEPTION_ACCESS_VIOLATION:         name = "ACCESS_VIOLATION"; break;
            case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    name = "ARRAY_BOUNDS_EXCEEDED"; break;
            case EXCEPTION_BREAKPOINT:               name = "BREAKPOINT"; break;
            case EXCEPTION_DATATYPE_MISALIGNMENT:    name = "DATATYPE_MISALIGNMENT"; break;
            case EXCEPTION_FLT_DENORMAL_OPERAND:     name = "FLT_DENORMAL_OPERAND"; break;
            case EXCEPTION_FLT_DIVIDE_BY_ZERO:       name = "FLT_DIVIDE_BY_ZERO"; break;
            case EXCEPTION_FLT_INEXACT_RESULT:       name = "FLT_INEXACT_RESULT"; break;
            case EXCEPTION_FLT_INVALID_OPERATION:    name = "FLT_INVALID_OPERATION"; break;
            case EXCEPTION_FLT_OVERFLOW:             name = "FLT_OVERFLOW"; break;
            case EXCEPTION_FLT_STACK_CHECK:          name = "FLT_STACK_CHECK"; break;
            case EXCEPTION_FLT_UNDERFLOW:            name = "FLT_UNDERFLOW"; break;
            case EXCEPTION_ILLEGAL_INSTRUCTION:      name = "ILLEGAL_INSTRUCTION"; break;
            case EXCEPTION_IN_PAGE_ERROR:            name = "IN_PAGE_ERROR"; break;
            case EXCEPTION_INT_DIVIDE_BY_ZERO:       name = "INT_DIVIDE_BY_ZERO"; break;
            case EXCEPTION_INT_OVERFLOW:             name = "INT_OVERFLOW"; break;
            case EXCEPTION_INVALID_DISPOSITION:      name = "INVALID_DISPOSITION"; break;
            case EXCEPTION_NONCONTINUABLE_EXCEPTION: name = "NONCONTINUABLE_EXCEPTION"; break;
            case EXCEPTION_PRIV_INSTRUCTION:         name = "PRIV_INSTRUCTION"; break;
            case EXCEPTION_SINGLE_STEP:              name = "SINGLE_STEP"; break;
            case EXCEPTION_STACK_OVERFLOW:           name = "STACK_OVERFLOW"; break;
        }
        crashLog << "Exception name: " << name << std::endl;
        crashLog << std::endl;
        crashLog.close();
    }

    // 2. Write minidump
    auto dumpPath = exeDir / "proJV_crash.dmp";
    HANDLE hDumpFile = CreateFileW(
        dumpPath.wstring().c_str(),
        GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hDumpFile != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei;
        mei.ThreadId          = GetCurrentThreadId();
        mei.ExceptionPointers = pExp;
        mei.ClientPointers    = FALSE;
        MiniDumpWriteDump(
            GetCurrentProcess(), GetCurrentProcessId(),
            hDumpFile, MiniDumpNormal, &mei, nullptr, nullptr);
        CloseHandle(hDumpFile);
    }

    // 3. Event Log
    HANDLE hEventLog = RegisterEventSourceW(nullptr, L"Application");
    if (hEventLog) {
        std::wstring msg = L"proJV.exe crashed with exception 0x" +
            std::to_wstring(pExp->ExceptionRecord->ExceptionCode);
        LPCWSTR strings[] = { msg.c_str() };
        ReportEventW(hEventLog, EVENTLOG_ERROR_TYPE, 0, 0, nullptr, 1, 0, strings, nullptr);
        DeregisterEventSource(hEventLog);
    }

    return EXCEPTION_EXECUTE_HANDLER;
}
#endif  // _WIN32

// --- main ------------------------------------------------------------------

int main(int argc, char** argv) {
    (void)argc; (void)argv;  // unused for now; argv kept for loguru init below

    // Install crash handler (Windows SEH)
#ifdef _WIN32
    GetModuleFileNameA(nullptr, crashExePath, MAX_PATH);
    SetUnhandledExceptionFilter(unhandledExceptionFilter);
#endif

    // Initialize logging (debug builds)
#ifndef PROJV_RELEASE
    {
        int logArgc = 1;
        char* logArgv[] = { const_cast<char*>("proJV"), nullptr };
        loguru::init(logArgc, logArgv);
        std::string logPath = std::string(crashExePath) + ".log";
        loguru::add_file(logPath.c_str(), loguru::Append, loguru::Verbosity_MAX);
        LOG_F(INFO, "=== proJV started (OpenGL+GLFW) ===");
    }
#endif

    // --- GUI Backend ----------------------------------------------------
    if (!g_backend.Init(1280, 800, "proJV - DeepSeek Agent")) {
        fprintf(stderr, "Failed to initialize GUI backend\n");
#ifdef _WIN32
        MessageBoxA(nullptr, "Failed to create OpenGL window.\n\n"
            "OpenGL 3.3 support is required.\n"
            "Please update your graphics driver.",
            "proJV - Error", MB_OK | MB_ICONERROR);
#endif
        return 1;
    }

    // --- Initialize app (config loaded here) ----------------------------
    g_app.initialize();

    // --- Initialize theme system AFTER config load ----------------------
    ThemeManager::instance().init(getThemeDir(), g_app.getConfigThemeName());

    // --- Set window title with API key status ---------------------------
    g_backend.SetTitle(g_app.hasApiKey() ? "proJV - DeepSeek Agent" : "proJV - [No API Key]");

    // --- Main loop ------------------------------------------------------
    bool done = false;
    auto lastSlowFrameLog = std::chrono::steady_clock::now();
    static constexpr auto kSlowFrameThreshold = std::chrono::milliseconds(50);
    static constexpr auto kSlowFrameLogInterval = std::chrono::seconds(2);
    int lastAgentState = -1;

    while (!done) {
        try {
            auto frameStart = std::chrono::steady_clock::now();

            // Start frame (GLFW events + ImGui new frame)
            g_backend.NewFrame();
            auto t1 = std::chrono::steady_clock::now();

            // Check for window close (GLFW or PostQuitMessage from app)
            if (g_backend.ShouldClose())
                done = true;
            if (done) break;

            // Render app UI
            g_app.render();
            auto t2 = std::chrono::steady_clock::now();

            // Apply theme clear color every frame
            ImVec4 cc = ThemeColors::toVec4(ThemeManager::instance().current().clearColor);
            g_backend.SetClearColor(cc.x, cc.y, cc.z, cc.w);

            // Render + present
            g_backend.Render();
            auto t4 = std::chrono::steady_clock::now();

#ifndef PROJV_RELEASE
            // -- Frame timing watchdog --
            auto frameElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(t4 - frameStart);
            if (frameElapsed >= kSlowFrameThreshold) {
                auto now = std::chrono::steady_clock::now();
                if (now - lastSlowFrameLog >= kSlowFrameLogInterval) {
                    lastSlowFrameLog = now;
                    auto renderTime = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1);
                    auto gpuTime    = std::chrono::duration_cast<std::chrono::milliseconds>(t4 - t2);
                    LOG_F(WARNING, "SLOW FRAME: total=%lld ms  render=%lld  gpu=%lld",
                        (long long)frameElapsed.count(),
                        (long long)renderTime.count(),
                        (long long)gpuTime.count());
                }
            }

            // -- State transition watchdog --
            int curAgentState = g_app.getAgentState();
            if (lastAgentState != 0 && curAgentState == 0) {
                auto renderTime = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1);
                auto gpuTime    = std::chrono::duration_cast<std::chrono::milliseconds>(t4 - t2);
                LOG_F(INFO, "STATE Idle ENTERED: total=%lld ms  render=%lld  gpu=%lld",
                    (long long)frameElapsed.count(),
                    (long long)renderTime.count(),
                    (long long)gpuTime.count());
            }
            lastAgentState = curAgentState;
#endif
        }
        catch (const std::exception& e) {
#ifndef PROJV_RELEASE
            if (auto* se = dynamic_cast<const std::system_error*>(&e)) {
                LOG_F(ERROR, "  => system_error: value=%d  category='%s'  code_msg='%s'  generic='%s'",
                    se->code().value(),
                    se->code().category().name(),
                    se->code().message().c_str(),
                    std::generic_category().message(se->code().value()).c_str());
            }
            if (auto* fe = dynamic_cast<const std::filesystem::filesystem_error*>(&e)) {
                LOG_F(ERROR, "  => filesystem_error: path1='%s'  path2='%s'",
                    fe->path1().string().c_str(),
                    fe->path2().string().c_str());
            }
            LOG_F(ERROR, "CRASH in main loop (std::exception): %s", e.what());
#endif
            try {
                std::string msg = "An unexpected error occurred:\n\n";
                msg += e.what();
                msg += "\n\nCheck the log file for details.";
#ifdef _WIN32
                MessageBoxA(nullptr, msg.c_str(), "proJV - Error", MB_OK | MB_ICONERROR);
#endif
            } catch (...) {}
        }
        catch (...) {
#ifndef PROJV_RELEASE
            LOG_F(ERROR, "CRASH in main loop (unknown exception)");
#endif
            try {
#ifdef _WIN32
                MessageBoxA(nullptr, "An unknown error occurred.\n\n"
                    "Check the log file for details.",
                    "proJV - Error", MB_OK | MB_ICONERROR);
#endif
            } catch (...) {}
        }
    }

    // --- Cleanup --------------------------------------------------------
    g_backend.Shutdown();
    return 0;
}
