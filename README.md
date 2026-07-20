# proJV — Windows DeepSeek GUI Agent

<p align="center">
  <a href="README_zh.md">简体中文</a> | <strong>English</strong>
</p>

<p align="center">
  <img src="assets/overview.png" alt="proJV overview" width="1280"/>
</p>

Project JV, stands for **Just Vibing** — a sleek, fast, fully AI-built Windows desktop agent for the DeepSeek API.

Built with **Dear ImGui + DirectX11 + libcurl**, C++20. **Zero package managers.** One `cmake --build` does it all.

> Every line of code in this project was written by an AI Agent. I just filed requests and clicked Approve.

---

## Why proJV

I'm an algorithm engineer who writes C++ and Python. I like my PC for gaming, and I like lightweight libraries.

Most AI coding tools feel bloated — flashy plugins, complex terminals, features I'll never use. I wanted something different: a **native Windows desktop app** that's fast, minimal, and actually helpful.

proJV is that tool. It reads your codebase, runs shell commands, edits files, searches the web, manages TODOs — all through a clean GUI. It can even **modify and compile itself**.

---

## Spotlight Features

### Multi-Role System Prompts

Drop a `.md` file into `projv_prompts/` and it instantly appears in the role selector. Built-in roles include **coder** (full tool access) and **designer** (read-only code analysis — shell and source edits locked out, outputs structured design docs via `md_file`). Switch mid-conversation without losing context.

<p align="center">
  <img src="assets/customize_system_prompt.png" alt="System prompts" width="720"/>
</p>

### Theme System

Two built-in presets (Obsidian & Light), plus 5 curated themes installable from `projv_theme/`. Every color is editable in a visual panel with live preview and JSON import/export. Your choice is auto-saved to `config.toml` and restored on next launch.

| Preview | Theme | Style |
|---------|-------|-------|
| <img src="assets/themes/obsidian.png" width="180"/> | **Obsidian** | Refined warm-dark (default) |
| <img src="assets/themes/light.png" width="180"/> | **Light** | Clean white (default) |
| <img src="assets/themes/forest.png" width="180"/> | **Forest** | Airy sage green, nature-inspired |
| <img src="assets/themes/artism_warm.png" width="180"/> | **Artism** | Warm caramel, cozy café aesthetic |
| <img src="assets/themes/monochrome_dark.png" width="180"/> | **Monochrome Dark** | Pure grayscale, zero color distraction |
| <img src="assets/themes/colorblind_safe.png" width="180"/> | **Colorblind Safe** | Blue-orange palette, accessible for common CVD types |
| <img src="assets/themes/vibrant_focus.png" width="180"/> | **Vibrant Focus** | High-contrast neon, strong visual blocks |

<p align="center">
  <img src="assets/customize_theme_color.png" alt="Theme editor" width="720"/>
  <br/><sub>The built-in visual editor — edit colors by category, preview changes in real time, and export/import JSON themes</sub>
</p>

---

## Features

| Feature | Description |
|---------|-------------|
| **AI tool orchestration** | read_file, write_file, edit_file, md_file, shell, file_search, web_search, fetch_url, todo — AI decides what to call and when |
| **Thinking display** | deepseek-reasoner chain-of-thought as collapsible cards |
| **Streaming Markdown** | Real-time SSE with custom renderer (code blocks, tables, links, headings) |
| **Context management** | Token estimation + smart compaction at pressure thresholds |
| **Session persistence** | SQLite auto-save, new/save/load conversation history |
| **Live status bar** | Model name, token counts (in+out), message count, tool calls, context pressure % |
| **Tool approval** | Destructive operations require user confirmation |
| **TODO panel** | AI-managed task list, real-time sidebar |

---

## Quick Start

### Build

Requires CMake and Visual Studio 2019+ (or any C++20 compiler).

```bash
# Open x64 Native Tools Command Prompt, then:
cd proJV
mkdir build && cd build
cmake .. -G Ninja -DCMAKE_CXX_COMPILER=cl -DCMAKE_BUILD_TYPE=Release
cmake --build .
```

A `clean_and_build.bat` is also provided.

Artifact: `build/proJV.exe` (~3 MB)

### Run

1. Get a DeepSeek API Key: [platform.deepseek.com](https://platform.deepseek.com/api_keys)
2. Double-click `proJV.exe`, enter Key → **Save & Connect**
3. Start chatting — drop custom prompts into `projv_prompts/` and themes into `projv_theme/`

---

## Architecture

```
proJV/
├── CMakeLists.txt
├── assets/                    # Fonts, screenshots, theme previews
├── projv_theme/               # Installable theme JSON files
├── external/                  # Static deps: imgui, json.hpp, toml.hpp, SQLiteCpp, libcurl, loguru
├── src/
│   ├── main.cpp               # WinMain + D3D11 + ImGui loop
│   ├── client/deepseek.*      # DeepSeek API (libcurl + SSE)
│   ├── core/                  # Agent, Session, Config, Storage, Prompts
│   ├── ui/                    # App, Chat, Settings, Markdown, Theme system
│   └── tools/                 # Shell, file, search, web, md_file, todo
└── build/
    └── proJV.exe              # ~3 MB
```

---

## Dependencies

| Library | Purpose | Source |
|---------|---------|--------|
| [Dear ImGui](https://github.com/ocornut/imgui) | GUI framework | `external/imgui/` |
| [SQLiteCpp](https://github.com/SRombauts/SQLiteCpp) | Database | `external/SQLiteCpp-3.3.3/` |
| [libcurl](https://curl.se/) | HTTP/HTTPS | `external/curl-8.21.0/` |
| [loguru](https://github.com/emilk/loguru) | Logging | `external/loguru.cpp` |
| [nlohmann/json](https://github.com/nlohmann/json) | JSON parsing | Single header |
| [toml++](https://github.com/marzer/tomlplusplus) | TOML parsing | Single header |
| DirectX 11 | GPU rendering | System built-in |

**No vcpkg / conan / npm / pip.**

---

## Credits

- [DeepSeek](https://deepseek.com)
- [Dear ImGui](https://github.com/ocornut/imgui)
- [SQLiteCpp](https://github.com/SRombauts/SQLiteCpp)
- [libcurl](https://curl.se/)
- [loguru](https://github.com/emilk/loguru)
- All open-source maintainers

---

## Buy me a coffee

If proJV saves you time or brings you joy — I'd appreciate a coffee ☕
*(China / WeChat only)*

<p align="left">
  <img src="assets/wechat_donate.jpg" alt="WeChat donate" width="200"/>
</p>
