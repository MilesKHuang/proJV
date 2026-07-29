// proJV - Minimal DeepSeek AI Agent
// Built with Dear ImGui + OpenGL 3.3 + GLFW
//
// S4+S5: crash handler moved to SystemUtil, process runner via IProcessRunner

#include "gui_backend.h"
#include "platform/isystem_util.h"
#include "platform/iprocess_runner.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>

#include <imgui.h>
#include <loguru.hpp>

#include "ui/app.h"
#include "ui/theme.h"
#include "core/config.h"

// --- Global state ----------------------------------------------------------
static App            g_app;
static GuiBackend     g_backend;
static IProcessRunner* g_procRunner = nullptr;

// --- main ------------------------------------------------------------------

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    // Init platform services
    ISystemUtil* sys = CreateSystemUtil();
    SystemUtil::Init(sys);
    sys->InstallCrashHandler();
    g_procRunner = CreateProcessRunner();

    // Init logging
#ifndef PROJV_RELEASE
    {
        int logArgc = 1;
        char* logArgv[] = { const_cast<char*>("proJV"), nullptr };
        loguru::init(logArgc, logArgv);
        std::string logPath = sys->GetExeDir() + "/proJV.log";
        loguru::add_file(logPath.c_str(), loguru::Append, loguru::Verbosity_MAX);
        LOG_F(INFO, "=== proJV started (OpenGL+GLFW) ===");
    }
#endif

    // GUI Backend
    if (!g_backend.Init(1280, 800, "proJV - DeepSeek Agent")) {
#ifdef _WIN32
        MessageBoxA(nullptr, "Failed to create OpenGL window.\n"
            "OpenGL 3.3 support is required.", "proJV - Error",
            MB_OK | MB_ICONERROR);
#endif
        return 1;
    }

    // Init app
    g_app.initialize(g_procRunner);
    ThemeManager::instance().init(getThemeDir(),
        g_app.getConfigThemeName());
    g_backend.SetTitle(g_app.hasApiKey()
        ? "proJV - DeepSeek Agent" : "proJV - [No API Key]");

    // Main loop
    bool done = false;
    while (!done) {
        try {
            g_backend.NewFrame();
            if (g_backend.ShouldClose()) { done = true; break; }

            g_app.render();
            ImVec4 cc = ThemeColors::toVec4(
                ThemeManager::instance().current().clearColor);
            g_backend.SetClearColor(cc.x, cc.y, cc.z, cc.w);
            g_backend.Render();
        }
        catch (const std::exception& e) {
            try {
                std::string msg = "Error: ";
                msg += e.what();
#ifdef _WIN32
                MessageBoxA(nullptr, msg.c_str(), "proJV", MB_OK | MB_ICONERROR);
#endif
            } catch (...) {}
        }
        catch (...) {}
    }

    g_backend.Shutdown();
    delete g_procRunner;
    return 0;
}
