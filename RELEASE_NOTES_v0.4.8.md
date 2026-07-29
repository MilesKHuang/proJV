# proJV v0.4.8 Release Notes

> Date: 2026-07-29
> Compare: `v0.4.6` -> `v0.4.8`

---

## Highlights

### Cross-Platform: Windows + Linux

proJV is now a dual-platform application. One codebase, one CMakeLists.txt, builds on both Windows (MSVC) and Linux (GCC).

| Platform | Output | Approx. Size |
|----------|--------|-------------|
| Windows | `build/proJV.exe` | ~3 MB |
| Linux | `build_linux/proJV_linux` | ~3 MB |

### D3D11 → OpenGL 3.3 + GLFW

The entire GUI layer was migrated from Direct3D 11 + Win32 to OpenGL 3.3 + GLFW. Both platforms share the same `GuiBackend` class. No visual difference — same ImGui rendering, same themes, same frame rate.

### WinHTTP → libcurl

All HTTP code moved from Windows-only WinHTTP to cross-platform libcurl. Windows builds curl from source (`external/curl-8.21.0`), Linux uses the system package. `web_search` now uses Bing instead of DuckDuckGo.

### Process & System Abstraction

Two new interfaces isolate platform-specific code:

| Interface | Windows | Linux |
|-----------|---------|-------|
| `IProcessRunner` | `CreateProcess` + Job Object | `fork` + `execvp` + `setpgid` |
| `ISystemUtil` | `GetModuleFileName` + SEH | `/proc/self/exe` + `sigaction` + `backtrace` |

Upper-level code (`shell_tool`, `python_tool_manager`, `config`, `main`) calls these interfaces, isolating platform-specific logic.

### Linux Build Available

```bash
cd proJV
mkdir build_linux && cd build_linux
cmake .. -DCMAKE_BUILD_TYPE=Release -DGLFW_BUILD_WAYLAND=OFF
make -j$(nproc)
```

Requires: `build-essential`, `cmake`, `libcurl4-openssl-dev`, `libgl1-mesa-dev`, X11 dev headers. GLFW is compiled from source — no `apt install libglfw3-dev` needed.

---

## Features (v0.4.6 carried forward)

| Feature | Status |
|---------|--------|
| PyTool system (auto-scan `projv_files/pytool/`) | ✅ |
| `diagram_tool` (Graphviz DOT renderer) | ✅ |
| Multi-role prompts (coder, designer, analyzer) | ✅ |
| Theme system (8 presets, visual editor, JSON import/export) | ✅ |
| TODO panel with completion summaries | ✅ |
| Reliable Cancel (kills entire process tree) | ✅ |
| Streaming resilience (300s timeout, 2 retries) | ✅ |

---

## Changes by Area

### GUI Backend

| Before (v0.4.6) | After (v0.4.8) |
|-----------------|---------------|
| D3D11 + Win32 in `main.cpp` | OpenGL 3.3 + GLFW in `gui_backend.cpp/.h` |
| `WinMain` + `WndProc` | Standard `main()` |
| `imgui_impl_win32` + `imgui_impl_dx11` | `imgui_impl_glfw` + `imgui_impl_opengl3` |
| Only Windows | Windows + Linux, same code |

### Network

| Before | After |
|--------|-------|
| WinHTTP (`winhttp.h`, `#pragma comment`) | libcurl (`curl/curl.h`) |
| DuckDuckGo HTML search | Bing API search |
| Windows-only | Cross-platform |

### Build System

| Before | After |
|--------|-------|
| Visual Studio-only CMake | Single CMakeLists.txt, `if(WIN32)`/`else()` branches |
| Hardcoded D3D11/DXGI/WinHTTP libs | OpenGL + GLFW + libcurl (platform-aware) |
| POST_BUILD copies `assets/` + `projv_files/` | Font copied at configure time, no POST_BUILD copy |
| `#ifdef CMAKE_BUILD_TYPE` for debug logs | Generator expression `$<$<CONFIG:Release>:PROJV_RELEASE>` |

### Platform Layer (New Files)

| File | Purpose |
|------|---------|
| `src/platform/iprocess_runner.h` | Process abstraction interface |
| `src/platform/process_runner_win.cpp` | Windows: `CreateProcess` + Job Object |
| `src/platform/process_runner_linux.cpp` | Linux: `fork/exec` + `killpg` |
| `src/platform/isystem_util.h` | System utility interface |
| `src/platform/system_util_win.cpp` | Windows: SEH crash handler, font path, URL open |
| `src/platform/system_util_linux.cpp` | Linux: `sigaction` crash handler, font search, `xdg-open` |
| `src/format_compat.h` | `std::format` polyfill for GCC |
| `src/platform_compat.h` | `localtime_s` polyfill, platform macros |

---

## Bug Fixes

| Issue | Fix |
|-------|-----|
| GLFW input sometimes lost on Windows | Added `glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_TRUE)` + `GLFW_SCALE_TO_MONITOR` |
| Chinese comments garbled under MSVC | Replaced with English comments |
| Linux: `std::format` unavailable on GCC 11 | Replaced with `snprintf` + `format_compat.h` polyfill |
| `Sleep()` Windows-only | Replaced 6 occurrences with `std::this_thread::sleep_for()` |

---

## Known Issues

| # | Issue | Impact |
|---|-------|--------|
| — | `python_tool_manager.cpp` Python auto-detection still uses `_popen`/`popen` | Cosmetic — the actual `executePyTool` path goes through `IProcessRunner` |
| — | Input text focus can be lost during typing on some configurations | Requires clicking back into input box |
| — | Ctrl+Enter shortcut may not always trigger Send | `InputTextMultiline` flags=0 consumes the key combination |

---

## File Changelog

### New
| File | Description |
|------|-------------|
| `src/gui_backend.cpp/.h` | OpenGL + GLFW GUI backend |
| `src/platform/iprocess_runner.h` | Process abstraction interface |
| `src/platform/process_runner_win.cpp` | Windows process implementation |
| `src/platform/process_runner_linux.cpp` | Linux process implementation |
| `src/platform/isystem_util.h` | System utility interface |
| `src/platform/system_util_win.cpp` | Windows system utilities |
| `src/platform/system_util_linux.cpp` | Linux system utilities |
| `src/format_compat.h` | `std::format` polyfill |
| `src/platform_compat.h` | Platform compatibility macros |
| `external/glfw/` | GLFW 3.4 source |
| `external/imgui/backends/imgui_impl_glfw.*` | ImGui GLFW backend |
| `external/imgui/backends/imgui_impl_opengl3.*` | ImGui OpenGL3 backend |
| `external/curl-8.21.0/` | libcurl source (Windows build) |

### Modified
| File | Key Changes |
|------|-------------|
| `src/main.cpp` | D3D11/Win32 removed; uses `GuiBackend` + standard `main()` |
| `src/tools/web_tools.cpp` | WinHTTP → libcurl; DuckDuckGo → Bing |
| `src/tools/shell_tool.cpp` | `#ifdef` branches removed; calls `IProcessRunner` |
| `src/core/config.cpp` | `GetModuleFileNameA` → `ISystemUtil::GetExeDir()` |
| `src/client/deepseek.cpp` | `Sleep()` → `std::this_thread::sleep_for()` |
| `src/ui/app.cpp` | `#include <windows.h>` wrapped in `#ifdef`; Exit → `glfwSetWindowShouldClose` |
| `src/ui/render_settings.cpp` | `#include <windows.h>` wrapped in `#ifdef` |
| `src/ui/theme_popup.cpp` | `#include <windows.h>` wrapped in `#ifdef` |
| `src/tools/shell_tool.h` | Signature changed to accept `IProcessRunner*` |
| `src/tools/python_tool_manager.cpp` | `executePyTool` migrated to `IProcessRunner`; Linux zenity file dialogs |
| `src/tools/python_tool_manager.h` | Interface updated for `IProcessRunner` |
| `src/tools/registry.cpp` | Unused `#include <windows.h>` removed |
| `src/ui/render_chat.cpp` | `ShellExecuteA` → `SystemUtil::Instance().OpenUrl()`; CJK font comments |
| `src/ui/app.h` | Added `pendingOpenChat_`/`pendingSaveChat_` for Linux deferred dialogs |
| `CMakeLists.txt` | Full cross-platform rewrite; genexpr for PROJV_RELEASE; POST_BUILD removed; font copy at configure time |
| `README.md` / `README_zh.md` | Bilingual overhaul: Why proJV, core features, pytool section, DeepSeek pricing |

### Removed
| File | Reason |
|------|--------|
| `external/imgui/backends/imgui_impl_win32.cpp/.h` | Replaced by GLFW backend |
| `external/imgui/backends/imgui_impl_dx11.cpp/.h` | Replaced by OpenGL3 backend |
