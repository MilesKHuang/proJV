# proJV -- A Native C++ AI Agent, DeepSeek GUI Desktop

<p align="center">
  <a href="README_zh.md">简体中文</a> | <strong>English</strong>
</p>

<p align="center">
  <img src="assets/overview.png" alt="proJV overview" width="1280"/>
</p>

**proJV = Project Just Vibing** -- a fast, native C++ desktop GUI Agent powered by DeepSeek.

Built with **Dear ImGui + GLFW/OpenGL3 + libcurl**, C++20. **Zero package managers.** One `cmake --build` gives you a ~3 MB binary.

> Every line of code in proJV was written by proJV.

---

## Why proJV

### 1. AI Native Embedded System -- C++ & GUI

AI Native Embedded System is a major direction for future Agents. Native C++ & GUI bring several advantages:

- **Hardware control extensibility** -- C++ can directly call system APIs, operate GPIO and peripherals with no intermediate layer.
- **C-end embedded is moving toward GUI, just like phones** -- graphical interfaces are the human-factor trend for consumer devices.
- **C++ deployment is lightweight** -- small binary, no runtime dependencies; complex AI capabilities can call Python via pytool.
- **Embedded scenarios are CPU-sensitive** -- too many interpreted languages consume compute resources.

proJV is built for this direction.

### 2. No-MCP

I believe you can build any MCP function by yourself.

### 3. DeepSeek Only (For Now)

It's just ridiculously cheap.

| Model | Input | Output |
|-------|-------|--------|
| deepseek-v4-flash | ¥1 / 1M tokens | ¥2 / 1M tokens |
| deepseek-v4-pro   | ¥3 / 1M tokens | ¥6 / 1M tokens |

---

## Core Features

### 1. Multi-Role System Prompts

Drop a `.md` file into `projv_files/prompts/` and it instantly appears in the role selector. Switch mid-conversation without losing context. Three built-in preset prompts:

| Role | Description |
|------|-------------|
| **coder** (default) | Full 11-tool access -- read, write, edit, shell, search, web, diagram, todo |
| **designer** | Read-only analysis mode -- shell and source edits locked out, outputs structured design docs via `md_file` |
| **analyzer** | Code analysis with `diagram_tool` support |

<p align="center">
  <img src="assets/customize_system_prompt.png" alt="System prompts" width="720"/>
</p>

### 2. Full Stack Customization -- Every Layer Is Yours

| Layer | What You Can Customize |
|-------|----------------------|
| **System Prompts** | Drop `.md` files into `projv_files/prompts/` -- instantly appear as selectable roles |
| **Compactor** | Control how context is compressed at pressure thresholds. Bring your own compaction strategy |
| **Python Tools (pytool)** | Write your own tool scripts in Python, placed in `projv_files/pytool/`. The agent discovers and calls them automatically |
| **Themes** | Visual editor with live preview, 8 presets, JSON import/export. Every color editable |
| **Config** | Everything in `config.toml` -- model, key, UI preferences. Plain text, no hidden state |

No locked-down "eco-system". **You own every knob.**

### 3. Python Tool Extension (pytool)

Drop a Python tool folder into `projv_files/pytool/` and the agent discovers it automatically. Each tool needs just two files:

```
projv_files/pytool/
└── my_tool/
    ├── tool.json    # name, description, parameters
    └── main.py      # reads JSON from stdin, prints result to stdout
```

**Minimal example** -- a tool that returns the current time:

`tool.json`:
```json
{
    "name": "get_time",
    "description": "Get the current system time.",
    "parameters": []
}
```

`main.py`:
```python
import json, sys, time
args = json.loads(sys.stdin.read())
print(time.strftime("%Y-%m-%d %H:%M:%S"))
```

**That's it.** The agent reads `tool.json` to know how to call your tool, invokes `main.py` with JSON args via stdin, and captures stdout as the result. Add `requirements.txt` for pip dependencies.

### 4. TODO -- Automatic Completion Summary

The AI manages a real-time TODO sidebar. When a task finishes, it doesn't just check a box -- **it writes a structured completion summary** with file paths, changes made, and verification results. The overview is archived to a permanent log. No more "what did the agent even do?"

### 5. Theme System

Two built-in presets (Obsidian & Light), plus 6 curated themes installable from `projv_files/theme/`. Every color is editable in a visual panel with live preview and JSON import/export. Your choice is auto-saved to `projv_files/config.toml` and restored on next launch.

| Preview | Theme | Style |
|---------|-------|-------|
| <img src="assets/themes/obsidian.png" width="180"/> | **Obsidian** | Refined warm-dark (default) |
| <img src="assets/themes/light.png" width="180"/> | **Light** | Clean white (default) |
| <img src="assets/themes/forest.png" width="180"/> | **Forest** | Airy sage green, nature-inspired |
| <img src="assets/themes/artism_warm.png" width="180"/> | **Artism** | Warm caramel, cozy cafe aesthetic |
| <img src="assets/themes/monochrome_dark.png" width="180"/> | **Monochrome Dark** | Pure grayscale, zero color distraction |
| <img src="assets/themes/colorblind_safe.png" width="180"/> | **Colorblind Safe** | Blue-orange palette, accessible for common CVD types |
| <img src="assets/themes/vibrant_focus.png" width="180"/> | **Vibrant Focus** | High-contrast neon, strong visual blocks |
| <img src="assets/themes/cyber_punk.png" width="180"/> | **Cyber Punk** | Neon yellow/pink/cyan on pure black, Cyberpunk 2077 style |

<p align="center">
  <img src="assets/customize_theme_color.png" alt="Theme editor" width="720"/>
  <br/><sub>The built-in visual editor -- edit colors by category, preview changes in real time, and export/import JSON themes</sub>
</p>

---

## Features

| Feature | Description |
|---------|-------------|
| **AI tool orchestration** | read_file, write_file, edit_file, md_file, exec_shell, grep_files, file_search, web_search, fetch_url, update_todo, diagram_tool -- AI decides what to call and when |
| **Thinking display** | deepseek-reasoner chain-of-thought as collapsible cards |
| **Streaming Markdown** | Real-time SSE with custom renderer (code blocks, tables, links, headings) |
| **Context management** | Token estimation + smart compaction at pressure thresholds |
| **Session persistence** | SQLite auto-save, new/save/load conversation history |
| **Live status bar** | Model name, token counts (in+out), message count, tool calls, context pressure % |
| **Tool approval** | Destructive operations require user confirmation |

---

## Quick Start

### Build

#### Windows

Requires CMake and Visual Studio 2019+ (or any C++20 compiler).

```bash
# Open x64 Native Tools Command Prompt, then:
cd proJV
mkdir build && cd build
cmake .. -G Ninja -DCMAKE_CXX_COMPILER=cl -DCMAKE_BUILD_TYPE=Release
cmake --build .
```

#### Linux

```bash
cd proJV
mkdir build_linux && cd build_linux
cmake .. -DCMAKE_BUILD_TYPE=Release -DGLFW_BUILD_WAYLAND=OFF
make -j$(nproc)
```

Artifact:
- Windows: `build/proJV.exe` (~3 MB)
- Linux: `build_linux/proJV_linux`

### Run

1. Get a DeepSeek API Key: [platform.deepseek.com](https://platform.deepseek.com/api_keys)
2. Launch proJV, enter Key -> **Save & Connect**
3. Start chatting -- drop custom prompts into `projv_files/prompts/`, themes into `projv_files/theme/`, Python tools into `projv_files/pytool/`

---

## Architecture

```
proJV/
├── CMakeLists.txt
├── assets/                    # Fonts, screenshots, theme previews
├── projv_files/               # Runtime data directory
│   ├── config.toml            # Configuration (auto-created on first save)
│   ├── prompts/               # System prompt .md files
│   ├── theme/                 # Installable theme JSON files
│   ├── pytool/                # Python tools (diagram_tool, etc.)
│   └── sessions/              # SQLite session databases
├── external/                  # Static deps: imgui, json.hpp, toml.hpp, SQLiteCpp, libcurl/glfw, loguru
├── src/
│   ├── main.cpp               # Entry point + ImGui loop
│   ├── client/deepseek.*      # DeepSeek API (libcurl + SSE)
│   ├── core/                  # Agent, Session, Config, Storage, Prompts
│   ├── ui/                    # App, Chat, Settings, Markdown, Theme system
│   └── tools/                 # Shell, file, search, web, md_file, todo
├── build/
│   └── proJV.exe              # ~3 MB (Windows)
└── build_linux/
    └── proJV_linux            # ~3 MB (Linux)
```

---

## Dependencies

| Library | Purpose | Source |
|---------|---------|--------|
| [Dear ImGui](https://github.com/ocornut/imgui) | GUI framework | `external/imgui/` |
| [SQLiteCpp](https://github.com/SRombauts/SQLiteCpp) | Database | `external/SQLiteCpp-3.3.3/` |
| [libcurl](https://curl.se/) | HTTP/HTTPS | `external/curl-8.21.0/` (Win) / system (Linux) |
| [GLFW](https://www.glfw.org/) | Window + OpenGL context | `external/glfw/` |
| [loguru](https://github.com/emilk/loguru) | Logging | `external/loguru.cpp` |
| [nlohmann/json](https://github.com/nlohmann/json) | JSON parsing | Single header |
| [toml++](https://github.com/marzer/tomlplusplus) | TOML parsing | Single header |

**No vcpkg / conan / npm / pip.** All dependencies are either bundled or system-provided.

---

## Credits

- [DeepSeek](https://deepseek.com)
- [Dear ImGui](https://github.com/ocornut/imgui)
- [SQLiteCpp](https://github.com/SRombauts/SQLiteCpp)
- [libcurl](https://curl.se/)
- [GLFW](https://www.glfw.org/)
- [loguru](https://github.com/emilk/loguru)
- All open-source maintainers

---

## Buy me a coffee

If proJV saves you time or brings you joy -- I'd appreciate a coffee
*(China / WeChat only)*

<p align="left">
  <img src="assets/wechat_donate.jpg" alt="WeChat donate" width="200"/>
</p>
