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

And yes, it's still a weekend project. But it's crossed the self-iteration threshold, and it keeps getting better.

---

## Spotlight Features

### Designer Workflow

Lock down destructive tools and let the AI produce structured design documents before writing code. Perfect for architecture reviews and large refactors.

<p align="center">
  <img src="assets/overview.png" alt="Designer workflow" width="800"/>
</p>

### Custom System Prompts

Drop a `.md` file into `projv_prompts/` — it instantly appears in the role selector. Switch between coder, designer, or your own custom persona mid-conversation without losing context.

<p align="center">
  <img src="assets/customize_system_prompt.png" alt="Custom system prompts" width="800"/>
</p>

### Theme System

7 curated themes built in, plus full customization. Pick from the menu, tweak every color in the visual editor, export/load JSON themes, and auto-persist your choice to `config.toml`.

<p align="center">
  <img src="assets/themes/obsidian.png" alt="Obsidian theme" width="230"/>
  <img src="assets/themes/forest.png" alt="Forest theme" width="230"/>
  <img src="assets/themes/artism_warm.png" alt="Artism theme" width="230"/>
  <br/>
  <sub><b>Obsidian</b> · <b>Forest</b> · <b>Artism</b> — 7 themes total</sub>
</p>

---

## Features

| Feature | Description |
|---------|-------------|
| **Multi-role prompts** | ComboBox switching between coder / designer / custom roles. Directory-driven — drop a `.md` into `projv_prompts/` and it auto-appears. |
| **Designer workflow** | Read-only code analysis mode: `read_file`/`grep_files` + `md_file` for design docs. Shell and source edits locked out. |
| **AI tool orchestration** | read_file, write_file, edit_file, md_file, shell, file_search, web_search, fetch_url, todo — AI decides what to call and when |
| **Theme system** | 7 built-in themes (Obsidian, Light, Forest, Artism, Monochrome, Colorblind Safe, Vibrant Focus) + visual editor + JSON import/export + config.toml persistence |
| **Thinking display** | deepseek-reasoner `reasoning_content` rendered as collapsible cards |
| **Streaming Markdown** | Real-time SSE streaming with custom Markdown renderer (code blocks, tables, links, headings) |
| **Context management** | Token estimation + smart compaction at pressure thresholds |
| **Session persistence** | SQLite auto-save, new/save/load conversation history |
| **Live status bar** | Model name, token counts (in+out), message count, tool calls, context pressure % |
| **Tool approval** | Destructive operations require user confirmation |
| **TODO panel** | AI-managed task list with real-time sidebar |

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
3. Start chatting — or drop custom prompts into `projv_prompts/` and themes into `projv_theme/`

---

## Architecture

```
proJV/
├── CMakeLists.txt
├─┬ assets/
│ ├── msyh.ttc                  # Microsoft YaHei CJK font
│ ├── overview.png
│ ├── customize_system_prompt.png
│ └─┬ themes/                   # Theme preview images
│   ├── obsidian.png
│   ├── forest.png
│   └── ...
├─┬ projv_theme/                # User-installable theme JSON files
│ └── *.json
├── external/                   # Static dependencies, no package manager
│   ├── imgui/                  # Dear ImGui
│   ├── json.hpp                # nlohmann/json (single header)
│   ├── toml.hpp                # toml++ (single header)
│   ├── SQLiteCpp-3.3.3/        # SQLite C++ wrapper
│   ├── curl-8.21.0/            # libcurl
│   └── loguru.cpp/hpp          # Logging
├─┬ src/
│ ├── main.cpp                  # WinMain + D3D11 + ImGui loop
│ ├── debug_log.h
│ ├─┬ client/                   # DeepSeek API client
│ │ └── deepseek.h/.cpp         # libcurl HTTP + SSE streaming
│ ├─┬ core/                     # Core logic
│ │ ├── agent.h/.cpp            # Agent loop (think → tool → reply)
│ │ ├── session.h/.cpp          # Session management + token estimation
│ │ ├── config.h/.cpp           # TOML config loader
│ │ ├── storage.h/.cpp          # SQLite persistence
│ │ ├── prompts_loader.cpp      # Directory-driven prompt presets
│ │ └── prompts.h
│ ├─┬ ui/                       # User interface
│ │ ├── app.h/.cpp              # Main app + window management
│ │ ├── render_chat.cpp         # Chat bubbles + Markdown + prompt combo
│ │ ├── render_settings.cpp     # Config + tool approval dialogs
│ │ ├── markdown_render.cpp     # Custom Markdown renderer
│ │ ├── theme.h/.cpp            # ThemeColors + ThemeManager (65-color system)
│ │ └── theme_popup.cpp         # Theme editor popup with live preview
│ └─┬ tools/                    # Agent-callable tools
│   ├── registry.h/.cpp         # Tool registry
│   ├── shell_tool.cpp          # Shell command execution
│   ├── file_tool.cpp           # File read/write/grep
│   ├── edit_file_tool.cpp      # Search-replace edit
│   ├── md_file_tool.cpp        # Markdown design doc writer (designer-only)
│   ├── file_search_tool.cpp    # File search
│   ├── web_tools.cpp           # Web search + fetch
│   └── todo_tool.cpp           # TODO management
└── build/
    └── proJV.exe               # Built artifact (~3 MB)
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
