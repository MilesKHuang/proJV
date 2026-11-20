// proJV studio -- window, GL context, ImGui init, main loop (design 08.5, D1).
//
// Platform backends, exactly as D1 decided:
//   Windows : imgui_impl_win32  (the only backend with a complete IME pipeline)
//             + a WGL OpenGL 3.3 core context, created by hand
//   Linux   : imgui_impl_sdl2   (GLFW has no IME support at all, which is a hard
//             defect, not a configuration problem) + an SDL GL context
//   Both    : imgui_impl_opengl3 for rendering, so there is ONE render path.
//
// Viewports stay OFF (multi-viewport focus bugs come from moving windows to a
// separate OS window). Docking is ON. The layout is the TUI's fixed one for now;
// the dockspace host arrives with the blueprint editor (08.7).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "imgui.h"

#include "core/config.h"
#include "debug_log.h"
#include "platform/cli_args.h"
#include "platform/iprocess_runner.h"
#include "platform/isystem_util.h"
#include "platform/log_files.h"
#include "studio_app.h"
#include "studio_theme.h"
#include "studio_ui.h"
#include "tui/theme_manager.h"

namespace {

// The TUI's own meta commands (--help / --version) are answered by the same
// parser, so `projv-studio --version` prints the same version string.
int printMetaIfAsked(int argc, char** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    std::string text;
    if (const auto exitCode = cli_args::parseMetaArgs(args, text)) {
        std::fputs(text.c_str(), stdout);
        std::fflush(stdout);
        return *exitCode;
    }
    return -1;
}

// `--dump-view-model [file]`: initialise, take one frame's snapshot, write it,
// exit. The TUI's event stream is the other half of the oracle; this is the
// window half (see docs/en/dev-guide/studio-gui.md).
bool dumpWanted(int argc, char** argv, std::string* outPath) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dump-view-model") == 0) {
            if (i + 1 < argc && argv[i + 1][0] != '-') *outPath = argv[i + 1];
            return true;
        }
    }
    return false;
}

} // namespace

#ifdef _WIN32
// ---------------------------------------------------------------------------
// Windows: a hand-written WGL context. ImGui's examples use GLFW for this, but
// GLFW is exactly what D1 rejects (no IME), so the 60 lines it would hide are
// written out here instead.
// ---------------------------------------------------------------------------
#include <windows.h>

#include <GL/gl.h>
#include <imm.h>
#include <shellapi.h>

#include "imgui_impl_opengl3.h"
#include "imgui_impl_win32.h"

// opengl32's ABI is GL 1.1; a 3.3 context needs the ARB entry points.
typedef HGLRC(WINAPI* PFN_wglCreateContextAttribsARB)(HDC, HGLRC, const int*);
typedef BOOL(WINAPI* PFN_wglSwapIntervalEXT)(int);

// imgui_impl_win32.h keeps this declaration inside a '#if 0' block on purpose
// (it refuses to drag <windows.h> into its own header), so the app copies it.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                             LPARAM lParam);
namespace {
HGLRC g_hglrc = nullptr;
HDC g_hdc = nullptr;
HWND g_hwnd = nullptr;
int g_width = 1280;
int g_height = 800;


// The IME composition and candidate windows follow the caret. imgui_impl_win32
// 1.93 does NOT install a Platform_SetImeDataFn, so without this the candidate
// window sits in a corner on a Chinese input method (test A1 in the design).
void setImeData(ImGuiContext*, ImGuiViewport*, ImGuiPlatformImeData* data) {
    if (!g_hwnd || !data) return;
    HIMC himc = ImmGetContext(g_hwnd);
    if (!himc) return;
    if (data->WantVisible) {
        COMPOSITIONFORM cf = {};
        cf.dwStyle = CFS_POINT;
        cf.ptCurrentPos.x = static_cast<LONG>(data->InputPos.x);
        cf.ptCurrentPos.y = static_cast<LONG>(data->InputPos.y);
        ImmSetCompositionWindow(himc, &cf);
        CANDIDATEFORM cand = {};
        cand.dwIndex = 0;
        cand.dwStyle = CFS_CANDIDATEPOS;
        cand.ptCurrentPos = cf.ptCurrentPos;
        ImmSetCandidateWindow(himc, &cand);
    }
    ImmReleaseContext(g_hwnd, himc);
}

LRESULT WINAPI wndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    const LRESULT handled = ImGui_ImplWin32_WndProcHandler(hwnd, msg, w, l);
    // Probe the raw message path: without this I could not tell "the key map is
    // wrong" from "the key never arrived" (it was the latter).
    static FILE* wndlog = nullptr;
    static bool wndlogInit = false;
    if (!wndlogInit && std::getenv("PROJV_STUDIO_DEBUG_LAYOUT")) {
        wndlogInit = true;
        wndlog = std::fopen("wnd.log", "w");
    }
    if (wndlog &&
        (msg == WM_KEYDOWN || msg == WM_CHAR || msg == WM_SYSKEYDOWN || msg == WM_SETFOCUS ||
         msg == WM_KILLFOCUS || msg == WM_ACTIVATE)) {
        std::fprintf(wndlog, "msg=%u wparam=%llu handled=%lld\n", msg,
                     static_cast<unsigned long long>(w), static_cast<long long>(handled));
        std::fflush(wndlog);
    }
    if (handled) return true;
    switch (msg) {
    case WM_SIZE:
        if (w != SIZE_MINIMIZED) {
            g_width = LOWORD(l);
            g_height = HIWORD(l);
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((w & 0xfff0) == SC_KEYMENU) return 0; // no alt-menu
        break;
    case WM_CLOSE:
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}

bool createWindow(const wchar_t* title) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"proJVStudio";
    RegisterClassExW(&wc);

    // Let Windows decide the geometry: create the window, then MAXIMIZE it. Every
    // hand-computed size I tried disagreed with what ImGui believed the client was
    // (the window ended up taller than the screen and the status bar was drawn off
    // the framebuffer). A maximized window makes the two agree by construction.
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                             CW_USEDEFAULT, 1200, 800, nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hwnd) return false;

    g_hdc = GetDC(g_hwnd);

    // A dummy context first: wglCreateContextAttribsARB only exists once a
    // context is current, which is the classic WGL bootstrap.
    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    const int pf = ChoosePixelFormat(g_hdc, &pfd);
    if (!pf || !SetPixelFormat(g_hdc, pf, &pfd)) return false;
    HGLRC dummy = wglCreateContext(g_hdc);
    if (!dummy) return false;
    wglMakeCurrent(g_hdc, dummy);

    auto createAttribs = reinterpret_cast<PFN_wglCreateContextAttribsARB>(
        wglGetProcAddress("wglCreateContextAttribsARB"));
    if (createAttribs) {
        const int attribs[] = {
            0x2091, 3,          // WGL_CONTEXT_MAJOR_VERSION_ARB
            0x2092, 3,          // WGL_CONTEXT_MINOR_VERSION_ARB
            0x9126, 0x00000001, // WGL_CONTEXT_PROFILE_MASK_ARB, CORE
            0,
        };
        g_hglrc = createAttribs(g_hdc, nullptr, attribs);
    }
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(dummy);
    if (!g_hglrc) g_hglrc = wglCreateContext(g_hdc); // last resort: legacy context
    if (!g_hglrc) return false;
    if (!wglMakeCurrent(g_hdc, g_hglrc)) return false;

    // One vsync'd swap per frame: a steady 60 fps that does not spin the CPU.
    auto swapInterval =
        reinterpret_cast<PFN_wglSwapIntervalEXT>(wglGetProcAddress("wglSwapIntervalEXT"));
    if (swapInterval) swapInterval(1);

    ShowWindow(g_hwnd, SW_SHOWMAXIMIZED);
    UpdateWindow(g_hwnd);
    return true;
}

void destroyWindow() {
    if (g_hglrc) {
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(g_hglrc);
        g_hglrc = nullptr;
    }
    if (g_hdc && g_hwnd) ReleaseDC(g_hwnd, g_hdc);
    if (g_hwnd) DestroyWindow(g_hwnd);
    g_hwnd = nullptr;
    g_hdc = nullptr;
}

// Map an ImGui key to the app's own key set. Everything else (text, Enter, mouse)
// belongs to the widget, exactly like the TUI's input component.
studio::GKey translateKey() {
    if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) return studio::GKey::F1;
    if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) return studio::GKey::F2;
    if (ImGui::IsKeyPressed(ImGuiKey_F3, false)) return studio::GKey::F3;
    if (ImGui::IsKeyPressed(ImGuiKey_F4, false)) return studio::GKey::F4;
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) return studio::GKey::F5;
    if (ImGui::IsKeyPressed(ImGuiKey_F6, false)) return studio::GKey::F6;
    if (ImGui::IsKeyPressed(ImGuiKey_F7, false)) return studio::GKey::F7;
    if (ImGui::IsKeyPressed(ImGuiKey_F8, false)) return studio::GKey::F8;
    if (ImGui::IsKeyPressed(ImGuiKey_F9, false)) return studio::GKey::F9;
    if (ImGui::IsKeyPressed(ImGuiKey_F10, false)) return studio::GKey::F10;
    if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) return studio::GKey::Tab;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) return studio::GKey::Escape;
    // Arrows are NOT gated on WantTextInput: a single-line field has no vertical
    // caret movement, and the "/" menu plus the chat both need them while the
    // prompt line is focused. That gate is why the menu could never move.
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false)) return studio::GKey::Up;
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, false)) return studio::GKey::Down;
    if (ImGui::IsKeyPressed(ImGuiKey_PageUp, false)) return studio::GKey::PageUp;
    if (ImGui::IsKeyPressed(ImGuiKey_PageDown, false)) return studio::GKey::PageDown;
    return studio::GKey::None;
}

} // namespace
#endif // _WIN32

#ifdef _WIN32
int runStudio(int argc, char** argv) {
    // A windowed binary has no console of its own. When it is started FROM a shell
    // (--version, --help, --dump-view-model), borrow the caller's so the output is
    // not swallowed.
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }
    if (const int meta = printMetaIfAsked(argc, argv); meta >= 0) return meta;
    std::string dumpPath;
    const bool dumpOnly = dumpWanted(argc, argv, &dumpPath);

    ISystemUtil* sys = CreateSystemUtil();
    SystemUtil::Init(sys);
    if (!dumpOnly) sys->InstallCrashHandler();
    IProcessRunner* runner = CreateProcessRunner();

    loguru::g_stderr_verbosity = loguru::Verbosity_OFF;
    loguru::g_colorlogtostderr = false;
    {
        const std::string logDir = getProjvDir() + "/logs";
        std::error_code ec;
        std::filesystem::create_directories(logDir, ec);
        if (!ec) log_files::pruneOldLogs(logDir, 7);
        const std::string logPath = log_files::todayLogPath(logDir);
#ifdef PROJV_RELEASE
        loguru::add_file(logPath.c_str(), loguru::Append, loguru::Verbosity_WARNING);
#else
        loguru::add_file(logPath.c_str(), loguru::Append, loguru::Verbosity_MAX);
#endif
    }

    studio::App app;
    if (!app.initialize(runner)) {
        std::fputs("proJV studio: initialize() failed (see projv_files/logs)\n", stderr);
        delete runner;
        return 1;
    }

    if (dumpOnly) {
        app.tick();
        const std::string json = app.dumpJson();
        if (dumpPath.empty()) {
            std::fwrite(json.data(), 1, json.size(), stdout);
        } else if (FILE* f = std::fopen(dumpPath.c_str(), "wb")) {
            std::fwrite(json.data(), 1, json.size(), f);
            std::fclose(f);
            std::printf("view model -> %s (%zu bytes)\n", dumpPath.c_str(), json.size());
        } else {
            std::fprintf(stderr, "cannot write %s\n", dumpPath.c_str());
        }
        app.shutdown();
        delete runner;
        return 0;
    }

    ImGui_ImplWin32_EnableDpiAwareness();
    if (!createWindow(L"proJV (studio)")) {
        std::fputs("proJV studio: cannot create the window / GL context\n", stderr);
        app.shutdown();
        delete runner;
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable; // D1: docking on
    // No viewports (D1): one OS window, no focus surprises.
    io.ConfigDpiScaleFonts = true;
    io.IniFilename = nullptr; // no imgui.ini: the layout is fixed for now
    io.LogFilename = nullptr;
    ImGui::GetPlatformIO().Platform_SetImeDataFn = setImeData;

    const float dpi = ImGui_ImplWin32_GetDpiScaleForHwnd(g_hwnd);
    app.setDpiScale(dpi > 0.1f ? dpi : 1.0f);
    if (std::getenv("PROJV_STUDIO_DEBUG_LAYOUT")) {
        RECT cr = {0, 0, 0, 0};
        GetClientRect(g_hwnd, &cr);
        RECT wr = {0, 0, 0, 0};
        GetWindowRect(g_hwnd, &wr);
        MONITORINFO mi = {sizeof(MONITORINFO)};
        GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
        std::fprintf(stderr,
                     "[studio] client=%ldx%ld window=%ldx%ld dpiScale=%.2f g=%dx%d "
                     "work=%ldx%ld@%ld,%ld\n",
                     cr.right - cr.left, cr.bottom - cr.top, wr.right - wr.left, wr.bottom - wr.top,
                     dpi, g_width, g_height, mi.rcWork.right - mi.rcWork.left,
                     mi.rcWork.bottom - mi.rcWork.top, mi.rcWork.left, mi.rcWork.top);
    }

    ImGui_ImplWin32_Init(g_hwnd);
    {
        // Diagnose the backend handshake: a silently failed Init leaves
        // BackendPlatformUserData null, and then EVERY key is dropped by the
        // WndProc handler while the window still paints.
        FILE* f = std::fopen("wnd.log", "a");
        if (f) {
            std::fprintf(f, "[init] hwnd=%p platform='%s' userdata=%p\n", (void*)g_hwnd,
                         ImGui::GetIO().BackendPlatformName ? ImGui::GetIO().BackendPlatformName
                                                            : "(null)",
                         ImGui::GetIO().BackendPlatformUserData);
            std::fclose(f);
        }
    }
    ImGui_ImplOpenGL3_Init("#version 130");

    studio_theme::applyStyle();
    // Metrics ONCE, scaled by the monitor DPI. ImGui does not scale its sizes with
    // the font, so without this a 150% display gets big glyphs in small padding --
    // every panel looked cramped for exactly that reason.
    studio_theme::applyMetrics(app.dpiScale());
    std::string fontErr;
    if (!studio_theme::loadFonts(app.dpiScale(), &fontErr))
        std::fprintf(stderr, "proJV studio: font: %s\n", fontErr.c_str());

    bool done = false;
    while (!done && !app.wantExit()) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // The app's key map runs BEFORE any widget is submitted, so Tab / Esc /
        // arrows / F-keys mean what they mean in the TUI even while the prompt
        // line has the keyboard.
        const studio::GKey key = translateKey();
        if (key != studio::GKey::None) app.handleKey(key);

        app.tick();
        studio::drawFrame(app);

        ImGui::Render();
        const ThemeColors& T = ThemeManager::instance().current();
        const ImU32 bg = studio_theme::fromHex(T.windowBg);
        // The viewport must match what ImGui BELIEVES, not what WM_SIZE last said:
        // under DPI virtualisation those two disagree, and everything below the
        // smaller of them is drawn outside the framebuffer (the window looked
        // half-empty while the layout log showed a full 1920px of content).
        const ImVec2 fb = ImGui::GetIO().DisplaySize;
        const ImVec2 fbScale = ImGui::GetIO().DisplayFramebufferScale;
        glViewport(0, 0, static_cast<int>(fb.x * fbScale.x), static_cast<int>(fb.y * fbScale.y));
        glClearColor(((bg >> IM_COL32_R_SHIFT) & 0xFF) / 255.0f,
                     ((bg >> IM_COL32_G_SHIFT) & 0xFF) / 255.0f,
                     ((bg >> IM_COL32_B_SHIFT) & 0xFF) / 255.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SwapBuffers(g_hdc);

        // 60 fps ceiling: the turn thread posts nothing, so the loop redraws and
        // picks up streamed text without an event to wait for.
        Sleep(8);
    }

    app.shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    destroyWindow();
    delete runner;
    return 0;
}

// A GUI binary: /SUBSYSTEM:WINDOWS (see CMakeLists), so no console window opens
// behind the app. Arguments still come from the command line, and runStudio()
// borrows the caller's console when it has output to print.
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> storage;
    storage.reserve(static_cast<size_t>(argc > 0 ? argc : 1));
    for (int i = 0; i < argc; ++i) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(n > 0 ? static_cast<size_t>(n - 1) : 0, '\0');
        if (n > 1) WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, s.data(), n, nullptr, nullptr);
        storage.push_back(std::move(s));
    }
    if (wide) LocalFree(wide);
    if (storage.empty()) storage.push_back("proJV_studio");
    std::vector<char*> argv;
    argv.reserve(storage.size());
    for (auto& s : storage) argv.push_back(s.data());
    return runStudio(static_cast<int>(argv.size()), argv.data());
}
#endif // _WIN32
