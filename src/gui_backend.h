// proJV -- GUI backend: OpenGL 3.3 + GLFW
// Unified implementation for Windows and Linux.
// Replaces the original D3D11 + Win32 window code.
#pragma once

#include <string>

struct GLFWwindow;

class GuiBackend {
public:
    GuiBackend() = default;
    ~GuiBackend();

    // Non-copyable, non-movable (owns GLFW window handle)
    GuiBackend(const GuiBackend&) = delete;
    GuiBackend& operator=(const GuiBackend&) = delete;

    // Create window + OpenGL context + ImGui context + backends.
    // Font is loaded here: projv_files/fonts/msyh.ttc -> system font -> builtin fallback.
    bool Init(int width, int height, const char* title);

    // Call at the start of each frame.
    // Polls GLFW events, starts ImGui frame, checks for window close.
    void NewFrame();

    // Call at the end of each frame.
    // Renders ImGui draw data to the OpenGL backbuffer, swaps buffers.
    void Render();

    // Destroy ImGui backends + context, GLFW window, GLFW state.
    void Shutdown();

    // Returns true when the user has requested the window to close.
    bool ShouldClose() const;

    // Returns the underlying GLFW window handle.
    GLFWwindow* GetNativeWindow() const { return m_window; }

    // Set the background clear color used by Render().
    void SetClearColor(float r, float g, float b, float a);

    // Set window title.
    void SetTitle(const char* title);

private:
    bool loadFonts(const std::string& exeDir, float dpiScale);

    GLFWwindow* m_window = nullptr;
    bool        m_initialized = false;
    float       m_clearColor[4] = {0.1f, 0.1f, 0.12f, 1.0f};
};
