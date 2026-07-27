// proJV -- GUI backend implementation: OpenGL 3.3 + GLFW + ImGui
#include "gui_backend.h"
#include "platform/isystem_util.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <cstdio>
#include <filesystem>
#include <string>

// --- GuiBackend ------------------------------------------------------------

GuiBackend::~GuiBackend() {
    if (m_initialized)
        Shutdown();
}

bool GuiBackend::Init(int width, int height, const char* title) {
    // ---- 1. Initialize GLFW ------------------------------------------------
    if (!glfwInit()) {
        fprintf(stderr, "[GuiBackend] glfwInit failed\n");
        return false;
    }

    // Request OpenGL 3.3 Core Profile
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);   // macOS requirement

    // Create window
    m_window = glfwCreateWindow(width, height, title, nullptr, nullptr);
    if (!m_window) {
        fprintf(stderr, "[GuiBackend] glfwCreateWindow failed\n");
        glfwTerminate();
        return false;
    }

    glfwMakeContextCurrent(m_window);
    glfwSwapInterval(1);  // VSync on

    // Detect DPI scale (high-DPI: fonts need to be physically larger)
    float xscale = 1.0f, yscale = 1.0f;
    glfwGetWindowContentScale(m_window, &xscale, &yscale);
    float dpiScale = (xscale + yscale) * 0.5f;

    // ---- 2. Initialize ImGui -----------------------------------------------
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // Inform ImGui about the scale for mouse/display
    io.DisplayFramebufferScale = ImVec2(xscale, yscale);
    io.IniFilename = nullptr;   // no .ini file

    // ---- 3. Load fonts -----------------------------------------------------
    if (!loadFonts(SystemUtil::Instance().GetExeDir(), dpiScale)) {
        fprintf(stderr, "[GuiBackend] Font loading failed, using builtin\n");
    }

    // ---- 4. Setup ImGui style ----------------------------------------------
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 0.0f;

    // ---- 5. Initialize ImGui backends for GLFW + OpenGL3 -------------------
    if (!ImGui_ImplGlfw_InitForOpenGL(m_window, true)) {
        fprintf(stderr, "[GuiBackend] ImGui_ImplGlfw_InitForOpenGL failed\n");
        ImGui::DestroyContext();
        glfwDestroyWindow(m_window);
        glfwTerminate();
        m_window = nullptr;
        return false;
    }

    if (!ImGui_ImplOpenGL3_Init("#version 330")) {
        fprintf(stderr, "[GuiBackend] ImGui_ImplOpenGL3_Init failed\n");
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(m_window);
        glfwTerminate();
        m_window = nullptr;
        return false;
    }

    m_initialized = true;
    return true;
}

void GuiBackend::NewFrame() {
    glfwPollEvents();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void GuiBackend::Render() {
    ImGui::Render();

    int display_w, display_h;
    glfwGetFramebufferSize(m_window, &display_w, &display_h);
    glViewport(0, 0, display_w, display_h);

    glClearColor(m_clearColor[0], m_clearColor[1], m_clearColor[2], m_clearColor[3]);
    glClear(GL_COLOR_BUFFER_BIT);

    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    glfwSwapBuffers(m_window);
}

void GuiBackend::Shutdown() {
    if (!m_initialized) return;

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    if (m_window) {
        glfwDestroyWindow(m_window);
        m_window = nullptr;
    }
    glfwTerminate();
    m_initialized = false;
}

bool GuiBackend::ShouldClose() const {
    return m_window && glfwWindowShouldClose(m_window);
}

void GuiBackend::SetClearColor(float r, float g, float b, float a) {
    m_clearColor[0] = r;
    m_clearColor[1] = g;
    m_clearColor[2] = b;
    m_clearColor[3] = a;
}

void GuiBackend::SetTitle(const char* title) {
    if (m_window)
        glfwSetWindowTitle(m_window, title);
}

// --- Font loading -----------------------------------------------------------

bool GuiBackend::loadFonts(const std::string& exeDir, float dpiScale) {
    ImGuiIO& io = ImGui::GetIO();

    // --- Strategy: bundled font -> system font -> builtin fallback ----------
    // (1) Try bundled font
    std::string bundled = exeDir + "/assets/msyh.ttc";
    ImFontConfig cfg;
    float fontSize = 17.0f * dpiScale;
    cfg.SizePixels = fontSize;
    if (std::filesystem::exists(bundled) &&
        io.Fonts->AddFontFromFileTTF(bundled.c_str(), fontSize, &cfg,
            io.Fonts->GetGlyphRangesChineseSimplifiedCommon()))
        return true;

    // (2) Try system font via ISystemUtil
    std::string sysPath = SystemUtil::Instance().GetSystemFontPath("msyh.ttc");
    if (!sysPath.empty() && std::filesystem::exists(sysPath) &&
        io.Fonts->AddFontFromFileTTF(sysPath.c_str(), fontSize, &cfg,
            io.Fonts->GetGlyphRangesChineseSimplifiedCommon()))
        return true;

    // (3) Fallback: ImGui builtin font
    cfg.SizePixels = 16.0f * dpiScale;
    io.Fonts->AddFontDefault(&cfg);
    return false;
}
