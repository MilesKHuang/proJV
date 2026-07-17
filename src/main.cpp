// proJV - Minimal DeepSeek AI Agent
// Built with Dear ImGui + DirectX11 + WinHTTP

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dbghelp.h>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include <loguru.hpp>
#include "ui/app.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

// --- Global state --------------------------------------------------
static App g_app;
static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

static HWND g_hwnd = nullptr;
static WNDCLASSEXW g_wc = {};

// --- Forward declarations ------------------------------------------
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Forward declarations
static void createRenderTarget();

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
        case WM_SIZE: {
            if (g_pd3dDevice && wParam != SIZE_MINIMIZED) {
                // Release old render target, resize swap chain, recreate
                if (g_mainRenderTargetView) {
                    g_mainRenderTargetView->Release();
                    g_mainRenderTargetView = nullptr;
                }
                g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
                createRenderTarget();
            }
            return 0;
        }
        case WM_SYSCOMMAND: {
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0; // Disable ALT application menu
            break;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_DPICHANGED:
            return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static void createRenderTarget() {
    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

static bool createDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL featureLevel;
    D3D_FEATURE_LEVEL featureLevelArray[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createFlags,
        featureLevelArray, 2, D3D11_SDK_VERSION,
        &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);

    if (FAILED(hr)) return false;

    createRenderTarget();
    return true;
}

static void cleanupDeviceD3D() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

// --- WinMain -------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow) {
    // =========================================================================
    // Install global crash handler: SEH unhandled exception filter.
    // Captures access violations, stack overflows, and other structured
    // exceptions that C++ try/catch cannot see.  Generates a minidump and
    // writes to the Windows Application Event Log.
    // =========================================================================
    static std::mutex crashMutex;           // prevent re-entrant crashes
    static char     crashExePath[MAX_PATH]; // cached for the filter
    GetModuleFileNameA(nullptr, crashExePath, MAX_PATH);

    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* pExp) -> LONG {
        std::lock_guard<std::mutex> lock(crashMutex);

        // ---- 1. Write crash details to a dedicated crash log ----
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

            // Decode common exception codes to human-readable names
            const char* name = "UNKNOWN";
            switch (code) {
                case EXCEPTION_ACCESS_VIOLATION:     name = "ACCESS_VIOLATION"; break;
                case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: name = "ARRAY_BOUNDS_EXCEEDED"; break;
                case EXCEPTION_BREAKPOINT:            name = "BREAKPOINT"; break;
                case EXCEPTION_DATATYPE_MISALIGNMENT: name = "DATATYPE_MISALIGNMENT"; break;
                case EXCEPTION_FLT_DENORMAL_OPERAND:  name = "FLT_DENORMAL_OPERAND"; break;
                case EXCEPTION_FLT_DIVIDE_BY_ZERO:    name = "FLT_DIVIDE_BY_ZERO"; break;
                case EXCEPTION_FLT_INEXACT_RESULT:    name = "FLT_INEXACT_RESULT"; break;
                case EXCEPTION_FLT_INVALID_OPERATION: name = "FLT_INVALID_OPERATION"; break;
                case EXCEPTION_FLT_OVERFLOW:          name = "FLT_OVERFLOW"; break;
                case EXCEPTION_FLT_STACK_CHECK:       name = "FLT_STACK_CHECK"; break;
                case EXCEPTION_FLT_UNDERFLOW:         name = "FLT_UNDERFLOW"; break;
                case EXCEPTION_ILLEGAL_INSTRUCTION:   name = "ILLEGAL_INSTRUCTION"; break;
                case EXCEPTION_IN_PAGE_ERROR:         name = "IN_PAGE_ERROR"; break;
                case EXCEPTION_INT_DIVIDE_BY_ZERO:    name = "INT_DIVIDE_BY_ZERO"; break;
                case EXCEPTION_INT_OVERFLOW:          name = "INT_OVERFLOW"; break;
                case EXCEPTION_INVALID_DISPOSITION:   name = "INVALID_DISPOSITION"; break;
                case EXCEPTION_NONCONTINUABLE_EXCEPTION: name = "NONCONTINUABLE_EXCEPTION"; break;
                case EXCEPTION_PRIV_INSTRUCTION:      name = "PRIV_INSTRUCTION"; break;
                case EXCEPTION_SINGLE_STEP:           name = "SINGLE_STEP"; break;
                case EXCEPTION_STACK_OVERFLOW:        name = "STACK_OVERFLOW"; break;
            }
            crashLog << "Exception name: " << name << std::endl;
            crashLog << std::endl;
            crashLog.close();
        }

        // ---- 2. Write minidump for post-mortem WinDbg analysis ----
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
                GetCurrentProcess(),
                GetCurrentProcessId(),
                hDumpFile,
                MiniDumpNormal,        // Small, fast — enough for stack + module list
                &mei,
                nullptr,
                nullptr);
            CloseHandle(hDumpFile);
        }

        // ---- 3. Report to Windows Application Event Log ----
        HANDLE hEventLog = RegisterEventSourceW(nullptr, L"Application");
        if (hEventLog) {
            std::wstring msg = L"proJV.exe crashed with exception 0x" +
                std::to_wstring(pExp->ExceptionRecord->ExceptionCode);
            LPCWSTR strings[] = { msg.c_str() };
            ReportEventW(hEventLog, EVENTLOG_ERROR_TYPE, 0, 0, nullptr, 1, 0, strings, nullptr);
            DeregisterEventSource(hEventLog);
        }

        return EXCEPTION_EXECUTE_HANDLER; // let the process terminate
    });

#ifndef PROJV_RELEASE
    // Initialize logging (debug builds only)
    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    int logArgc = 1;
    char* logArgv[] = {exePath, nullptr};  // argv[argc] must be nullptr per C standard
    loguru::init(logArgc, logArgv);
    std::string logPath = std::string(exePath) + ".log";
    loguru::add_file(logPath.c_str(), loguru::Append, loguru::Verbosity_MAX);
    LOG_F(INFO, "=== proJV started ===");
#endif

    // Create window
    g_wc = { sizeof(g_wc), CS_CLASSDC, WndProc, 0L, 0L, hInst, nullptr, nullptr, nullptr, nullptr, L"proJV", nullptr };
    RegisterClassExW(&g_wc);
    g_hwnd = CreateWindowW(g_wc.lpszClassName, L"proJV - DeepSeek Agent",
        WS_OVERLAPPEDWINDOW, 100, 100, 1280, 800, nullptr, nullptr, g_wc.hInstance, nullptr);

    if (!g_hwnd) return 1;

    // Initialize Direct3D
    if (!createDeviceD3D(g_hwnd)) {
        cleanupDeviceD3D();
        UnregisterClassW(g_wc.lpszClassName, g_wc.hInstance);
        return 1;
    }

    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr; // no .ini file

    // Font -- load CJK-capable system font for Chinese/ASCII mixed content
    // Resolve font path relative to the executable directory (not CWD)
    char fontPath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, fontPath, MAX_PATH);
    std::filesystem::path exeDir = std::filesystem::path(fontPath).parent_path();
    std::string fontFullPath = (exeDir / "assets/msyh.ttc").string();

    ImFontConfig fontCfg;
    fontCfg.SizePixels = 17.0f;
    // Try bundled font first
    if (std::filesystem::exists(fontFullPath) &&
        io.Fonts->AddFontFromFileTTF(
            fontFullPath.c_str(), 17.0f, &fontCfg,
            io.Fonts->GetGlyphRangesChineseSimplifiedCommon()))
    {
#ifndef PROJV_RELEASE
        LOG_F(INFO, "Loaded font: %s", fontFullPath.c_str());
#endif
    }
    else
    {
        // Fallback: try finding msyh.ttc in system font directory
        std::string sysFontPath = "C:\\Windows\\Fonts\\msyh.ttc";
        if (std::filesystem::exists(sysFontPath) &&
            io.Fonts->AddFontFromFileTTF(
                sysFontPath.c_str(), 17.0f, &fontCfg,
                io.Fonts->GetGlyphRangesChineseSimplifiedCommon()))
        {
#ifndef PROJV_RELEASE
            LOG_F(INFO, "Loaded system font: %s", sysFontPath.c_str());
#endif
        }
        else
        {
            fontCfg.SizePixels = 16.0f;
            io.Fonts->AddFontDefault(&fontCfg);
#ifndef PROJV_RELEASE
            LOG_F(WARNING, "No CJK font found, using default");
#endif
        }
    }

    // Setup style
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 0.0f;

    // Setup Platform/Renderer backends
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // Initialize app
    g_app.initialize();

    // Set window title with status
    SetWindowTextW(g_hwnd, g_app.hasApiKey() ? L"proJV - DeepSeek Agent" : L"proJV - [No API Key]");

    // --- Main loop --------------------------------------------------
    bool done = false;
    auto lastSlowFrameLog = std::chrono::steady_clock::now();
    static constexpr auto kSlowFrameThreshold = std::chrono::milliseconds(50);
    static constexpr auto kSlowFrameLogInterval = std::chrono::seconds(2);
    // Track state transitions to force-log the first frame after returning to Idle
    int  lastAgentState = -1;

    while (!done) {
        try {
            auto frameStart = std::chrono::steady_clock::now();

            // Poll and handle messages
            auto t0 = std::chrono::steady_clock::now();
            MSG msg;
            while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
                if (msg.message == WM_QUIT) done = true;
            }
            if (done) break;

            // Handle window being minimized
            if (IsIconic(g_hwnd)) {
                Sleep(10);
                continue;
            }

            // Start the Dear ImGui frame
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            // Render app
            auto t1 = std::chrono::steady_clock::now();
            g_app.render();
            auto t2 = std::chrono::steady_clock::now();

            // Rendering
            ImGui::Render();
            auto t3 = std::chrono::steady_clock::now();
            const float clearColor[4] = { 0.08f, 0.08f, 0.10f, 1.00f };
            g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
            g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clearColor);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

            g_pSwapChain->Present(1, 0); // VSync on
            auto t4 = std::chrono::steady_clock::now();

#ifndef PROJV_RELEASE
            // -- Frame timing watchdog: log slow frames to catch UI stalls --
            auto frameElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(t4 - frameStart);
            if (frameElapsed >= kSlowFrameThreshold) {
                auto now = std::chrono::steady_clock::now();
                if (now - lastSlowFrameLog >= kSlowFrameLogInterval) {
                    lastSlowFrameLog = now;
                    auto msgTime  = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0);
                    auto renderTime = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1);
                    auto gpuTime   = std::chrono::duration_cast<std::chrono::milliseconds>(t4 - t3);
                    LOG_F(WARNING, "SLOW FRAME: total=%lld ms  msg=%lld  render=%lld  gpu=%lld",
                        (long long)frameElapsed.count(),
                        (long long)msgTime.count(),
                        (long long)renderTime.count(),
                        (long long)gpuTime.count());
                }
            }

            // -- State transition watchdog: force-log first frames after busy→Idle --
            int curAgentState = g_app.getAgentState();
            if (lastAgentState != 0 && curAgentState == 0) {
                    auto msgTime  = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0);
                    auto renderTime = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1);
                    auto gpuTime   = std::chrono::duration_cast<std::chrono::milliseconds>(t4 - t3);
                LOG_F(INFO, "STATE Idle ENTERED: total=%lld ms  msg=%lld  render=%lld  gpu=%lld",
                        (long long)frameElapsed.count(),
                        (long long)msgTime.count(),
                        (long long)renderTime.count(),
                        (long long)gpuTime.count());
            }
            lastAgentState = curAgentState;
#endif
        }
        catch (const std::exception& e) {
#ifndef PROJV_RELEASE
            // -- Type-specific diagnostics for debugging --
            // 1. std::system_error → log error code + category
            if (auto* se = dynamic_cast<const std::system_error*>(&e)) {
                LOG_F(ERROR, "  => system_error: value=%d  category='%s'  code_msg='%s'  generic='%s'",
                    se->code().value(),
                    se->code().category().name(),
                    se->code().message().c_str(),
                    std::generic_category().message(se->code().value()).c_str());
            }
            // 2. std::filesystem::filesystem_error → log paths
            if (auto* fe = dynamic_cast<const std::filesystem::filesystem_error*>(&e)) {
                LOG_F(ERROR, "  => filesystem_error: path1='%s'  path2='%s'",
                    fe->path1().string().c_str(),
                    fe->path2().string().c_str());
            }

            LOG_F(ERROR, "CRASH in main loop (std::exception): %s", e.what());
#endif
            // Try to log additional info and notify user
            try {
                std::string msg = "An unexpected error occurred:\n\n";
                msg += e.what();
                msg += "\n\nCheck the log file for details.";
                MessageBoxA(g_hwnd, msg.c_str(), "proJV - Error", MB_OK | MB_ICONERROR);
            } catch (...) {}
            // Don't crash -- continue the loop (the error might be transient)
            // If a second exception happens immediately, the window will freeze
            // but the log will have the evidence.
        }
        catch (...) {
#ifndef PROJV_RELEASE
            LOG_F(ERROR, "CRASH in main loop (unknown exception)");
#endif
            try {
                MessageBoxA(g_hwnd, "An unknown error occurred.\n\nCheck the log file for details.",
                    "proJV - Error", MB_OK | MB_ICONERROR);
            } catch (...) {}
        }
    }

    // --- Cleanup ----------------------------------------------------
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    cleanupDeviceD3D();
    DestroyWindow(g_hwnd);
    UnregisterClassW(g_wc.lpszClassName, g_wc.hInstance);

    return 0;
}