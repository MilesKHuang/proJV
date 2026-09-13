# proJV -- A Native C++ AI Agent for the Terminal (TUI)

<p align="center">
  <a href="README_zh.md">简体中文</a> | <strong>English</strong>
</p>

**proJV = Project Just Vibing** -- a fast, native C++ terminal AI agent powered by DeepSeek.

Built with **FTXUI + libcurl**, C++20. **Zero package managers.** One `cmake --build` gives you a ~3 MB binary.

> Every line of code in proJV was written by proJV.

<!-- Demo GIF placeholder: add `assets/demo.gif` once recorded. -->

---

## Why proJV

### 1. AI Native Embedded System -- C++ & TUI

AI Native Embedded System is a major direction for future Agents. Native C++ and a terminal-first UI bring several advantages:

- **Hardware control extensibility** -- C++ can directly call system APIs, operate GPIO and peripherals with no intermediate layer.
- **C++ deployment is lightweight** -- small binary, no runtime dependencies; complex AI capabilities can call Python via pytool.
- **Terminal UI runs anywhere** -- SSH, tmux, headless boxes, Windows Terminal; no GPU or window system required.
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

Drop a `.md` file into `projv_files/prompts/` and it instantly appears as a selectable role. Switch roles with **Tab** mid-conversation without losing context. Three built-in presets:

| Role | Description |
|------|-------------|
| **coder** (default) | Full 11-tool access -- read, write, edit, shell, search, web, diagram, todo |
| **designer** | Read-only analysis mode -- shell and source edits locked out, outputs structured design docs via `md_file` |
| **analyzer** | Code analysis with `diagram_tool` support |

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

The AI manages a real-time TODO panel. When a task finishes, it doesn't just check a box -- **it writes a structured completion summary** with file paths, changes made, and verification results. No more "what did the agent even do?"

### 5. Theme System

Two built-in presets (Obsidian & Light), plus 6 curated themes installable from `projv_files/theme/`. Every color is editable in a visual editor with live preview and JSON import/export. Your choice is auto-saved to `projv_files/config.toml` and restored on next launch.

---

## Features

| Feature | Description |
|---------|-------------|
| **AI tool orchestration** | read_file, write_file, edit_file, md_file, exec_shell, grep_files, file_search, web_search, fetch_url, update_todo, diagram_tool -- AI decides what to call and when |
| **Thinking display** | deepseek-reasoner chain-of-thought, collapsed by default with a one-line summary + trailing window (F8) |
| **Streaming Markdown** | Real-time SSE with custom renderer (code blocks, tables, links, headings) |
| **CJK soft-wrap** | Chat bubbles and the input box wrap long lines and re-flow on resize |
| **Safe shutdown** | SQLite WAL is checkpointed and closed on window close / Ctrl+C |
| **Context management** | Token estimation + smart compaction at pressure thresholds |
| **Session persistence** | SQLite auto-save; open history from the sessions picker |
| **Live status bar** | Model, role, token counts, cost estimate, context pressure % |
| **Tool approval** | Destructive operations require user confirmation |

---

## Keyboard Shortcuts

| Key | Action |
|-----|--------|
| `F1` | About |
| `F2` | Configuration |
| `F3` | New chat |
| `F5` | Open session (picker from `projv_files/sessions/`) |
| `F6` | Switch theme |
| `F7` | Theme editor |
| `F8` | Toggle reasoning (collapsed summary ↔ trailing window) |
| `F9` | Toggle TODO panel |
| `F10` | Switch model (fetched from the API) |
| `Tab` | Cycle system-prompt role |
| `↑` / `↓` | Scroll chat by row |
| `PgUp` / `PgDn` | Scroll chat by page |
| `Enter` | Send message |
| `Esc` | Cancel turn (busy) / quit (idle) |

Quick commands: `/help`, `/workspace`, `/clear`, `/compress`.

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

> Note: if Windows Defender quarantines `proJV_tui.exe` (unsigned binary), add the build directory to exclusions:
> `Add-MpPreference -ExclusionPath "D:\ws\proJV\build"` (admin PowerShell).

#### Linux

```bash
cd proJV
mkdir build_linux && cd build_linux
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

Artifact: `proJV_tui` (~3 MB) on both platforms.

### Run

1. Get a DeepSeek API Key: [platform.deepseek.com](https://platform.deepseek.com/api_keys)
2. Launch `proJV_tui` in a terminal (Windows Terminal recommended), enter Key -> **Save & Connect**
3. Start chatting -- drop custom prompts into `projv_files/prompts/`, themes into `projv_files/theme/`, Python tools into `projv_files/pytool/`

---

## Architecture

```
proJV/
├── CMakeLists.txt
├── projv_files/               # Runtime data directory
│   ├── config.toml            # Configuration (auto-created on first save)
│   ├── prompts/               # System prompt .md files
│   ├── theme/                 # Installable theme JSON files
│   ├── pytool/                # Python tools (diagram_tool, etc.)
│   └── sessions/              # SQLite session databases
├── external/                  # Static deps: FTXUI, SQLiteCpp, libcurl, json.hpp, toml.hpp, loguru
├── src/
│   ├── tui/main_tui.cpp       # Entry point + FTXUI event loop
│   ├── tui/                   # App, chat view, markdown, config, theme, status bar, todo
│   ├── client/deepseek.*      # DeepSeek API (libcurl + SSE)
│   ├── core/                  # Agent, Session, Config, Storage, Prompts
│   ├── platform/              # Windows/Linux process & system abstraction
│   └── tools/                 # Shell, file, search, web, md_file, todo, pytool
└── tests/                     # doctest unit + FTXUI golden tests
```

---

## Dependencies

| Library | Purpose | Source |
|---------|---------|--------|
| [FTXUI](https://github.com/ArthurSonzogni/FTXUI) | Terminal UI framework | `external/FTXUI-7.0.3/` |
| [SQLiteCpp](https://github.com/SRombauts/SQLiteCpp) | Database | `external/SQLiteCpp-3.3.3/` |
| [libcurl](https://curl.se/) | HTTP/HTTPS | `external/curl-8.21.0/` (Win) / system (Linux) |
| [loguru](https://github.com/emilk/loguru) | Logging | `external/loguru.cpp` |
| [nlohmann/json](https://github.com/nlohmann/json) | JSON parsing | Single header |
| [toml++](https://github.com/marzer/tomlplusplus) | TOML parsing | Single header |

**No vcpkg / conan / npm / pip.** All dependencies are either bundled or system-provided.

---

## Credits

- [DeepSeek](https://deepseek.com)
- [FTXUI](https://github.com/ArthurSonzogni/FTXUI)
- [SQLiteCpp](https://github.com/SRombauts/SQLiteCpp)
- [libcurl](https://curl.se/)
- [loguru](https://github.com/emilk/loguru)
- All open-source maintainers

---

## Buy me a coffee

If proJV saves you time or brings you joy -- I'd appreciate a coffee
*(China / WeChat only)*

<p align="left">
  <img src="assets/wechat_donate.jpg" alt="WeChat donate" width="200"/>
</p>
