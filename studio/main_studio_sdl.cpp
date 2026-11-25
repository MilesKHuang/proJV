// proJV studio -- the Linux platform half (design 08.5, D1).
//
// SDL2 rather than GLFW, and that is a hard requirement, not a preference: GLFW
// has no IME support at all (no preedit, no candidate window positioning), so a
// Chinese user could not type into a GLFW window. SDL2 has had a stable ABI since
// 2013, which is also why the baseline Ubuntu 22.04 in docs/en/user-guide/01-install.md
// can use the distribution package (libsdl2-dev) instead of a vendored copy.
//
// Everything below mirrors studio/main_studio.cpp; the only differences are the
// window and event calls. Both call studio::App and studio::drawFrame, so the
// platform halves cannot drift apart in behaviour.
#include <cstdio>
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
#include "tui/terminal.h"
#include "studio_ui.h"
#include "tui/theme_manager.h"

#include <SDL.h>
#include <SDL_opengl.h>

#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl2.h"

namespace {

int printMetaIfAsked(int argc, char** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
        args.emplace_back(argv[i]);
    std::string text;
    if (const auto exitCode = cli_args::parseMetaArgs(args, text)) {
        std::fputs(text.c_str(), stdout);
        std::fflush(stdout);
        return *exitCode;
    }
    return -1;
}

bool dumpWanted(int argc, char** argv, std::string* outPath) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dump-view-model") == 0) {
            if (i + 1 < argc && argv[i + 1][0] != '-') *outPath = argv[i + 1];
            return true;
        }
    }
    return false;
}

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
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false) && !ImGui::GetIO().WantTextInput)
        return studio::GKey::Up;
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) && !ImGui::GetIO().WantTextInput)
        return studio::GKey::Down;
    if (ImGui::IsKeyPressed(ImGuiKey_PageUp, false) && !ImGui::GetIO().WantTextInput)
        return studio::GKey::PageUp;
    if (ImGui::IsKeyPressed(ImGuiKey_PageDown, false) && !ImGui::GetIO().WantTextInput)
        return studio::GKey::PageDown;
    return studio::GKey::None;
}

} // namespace

int main(int argc, char** argv) {
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
        }
        app.shutdown();
        delete runner;
        return 0;
    }

    // Resource sampling: the window owns it, the headless paths never start it.
    app.startSampling();

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "proJV studio: SDL_Init: %s\n", SDL_GetError());
        app.shutdown();
        delete runner;
        return 1;
    }

    const char* glslVersion = "#version 130";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    SDL_Window* window =
        SDL_CreateWindow("proJV (studio)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280,
                         800, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) {
        std::fprintf(stderr, "proJV studio: CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        app.shutdown();
        delete runner;
        return 1;
    }
    SDL_GLContext gl = SDL_GL_CreateContext(window);
    if (!gl) {
        std::fprintf(stderr, "proJV studio: GL context: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        app.shutdown();
        delete runner;
        return 1;
    }
    SDL_GL_MakeCurrent(window, gl);
    SDL_GL_SetSwapInterval(1); // vsync

    // IME: SDL_TEXTEDITING carries the preedit and SDL_TEXTINPUT the committed
    // characters. Starting text input is what makes a Chinese input method work
    // at all in an SDL window (test A2 in the design doc).
    SDL_StartTextInput();

    float scale = 1.0f;
    {
        const int display = SDL_GetWindowDisplayIndex(window);
        float ddpi = 0.0f, hdpi = 0.0f, vdpi = 0.0f;
        if (display >= 0 && SDL_GetDisplayDPI(display, &ddpi, &hdpi, &vdpi) == 0 && hdpi > 0.0f)
            scale = hdpi / 96.0f;
    }
    app.setDpiScale(scale);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // Same rule as the Win32 front-end: the studio scales its own metrics
    // (applyMetrics) and sets FontScaleDpi (loadFonts), so ImGui must not scale the
    // atlas a second time. With both on, a 150% display drew every glyph at 2.25x
    // inside a layout built for 1.5x: text ran past its box and was clipped.
    io.ConfigDpiScaleFonts = false;
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    ImGui_ImplSDL2_InitForOpenGL(window, gl);
    ImGui_ImplOpenGL3_Init(glslVersion);

    studio_theme::applyStyle();
    studio_theme::applyMetrics(app.dpiScale());
    std::string fontErr;
    if (!studio_theme::loadFonts(app.dpiScale(), &fontErr))
        std::fprintf(stderr, "proJV studio: font: %s\n", fontErr.c_str());

    bool done = false;
    while (!done && !app.wantExit()) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT) done = true;
            if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE &&
                e.window.windowID == SDL_GetWindowID(window))
                done = true;
        }
        if (done) break;

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        const studio::GKey key = translateKey();
        if (key != studio::GKey::None) app.handleKey(key);

        app.tick();
        // Same cue as the Win32 half and the TUI (see main_studio.cpp).
        if (app.takeBeep()) tui::beepIdle();
        studio::drawFrame(app);

        ImGui::Render();
        const ThemeColors& T = ThemeManager::instance().current();
        const ImU32 bg = studio_theme::fromHex(T.windowBg);
        int w = 0, h = 0;
        SDL_GL_GetDrawableSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(((bg >> IM_COL32_R_SHIFT) & 0xFF) / 255.0f,
                     ((bg >> IM_COL32_G_SHIFT) & 0xFF) / 255.0f,
                     ((bg >> IM_COL32_B_SHIFT) & 0xFF) / 255.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    app.shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    delete runner;
    return 0;
}
