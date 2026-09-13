# proJV v0.5.0 Release Notes

> Date: 2026-09-13
> Compare: `v0.4.8` -> `v0.5.0`

---

## Highlights

### ImGui/GLFW → FTXUI: a terminal-first UI

The entire frontend moved from a Dear ImGui + GLFW/OpenGL3 window to a native
**FTXUI** terminal UI. proJV is now a real TUI:

- No GPU or window-system dependency — runs over SSH, tmux, headless boxes and Windows Terminal.
- Same ~3 MB single binary, same backend (client/core/tools/platform) frozen during the migration.
- The legacy `src/ui/` (ImGui) and `external/glfw/` are removed; the new frontend lives in `src/tui/`.

### CJK-aware soft-wrap everywhere

Chat bubbles **and the input box** soft-wrap long lines and re-flow on resize.
Fullwidth (CJK) characters break per-character, ASCII words stay together, and
over-long words split so nothing is ever clipped. Long pasted text stays fully
visible and editable in the input box.

### Reasoning: collapsed by default with a trailing window

`deepseek-reasoner` chains-of-thought keep growing (tens of thousands of
characters). Reasoning is now:

- **collapsed by default** — a one-line summary shows char/line count plus the last line preview;
- **expanded / streaming** — shows only the last 18 lines (a trailing window) so the chat never floods and every streaming redraw stays cheap;
- toggled globally with **F8**.

### Status bar & theme contrast

The status bar renders per-segment theme colors on a menu-bar background, and
the bottom hint bar uses compact colored keycaps instead of a long text line.
Markdown table headers get their theme background back, and table borders use a
higher-contrast color.

### Graceful database close on window close / Ctrl+C

A cross-platform exit handler (`ISystemUtil::InstallExitHandler`) checkpoints
the SQLite WAL and closes both DB connections when the process is asked to
terminate — Windows `CTRL_CLOSE_EVENT`/`Ctrl+C`, Linux `SIGHUP`/`SIGINT`/
`SIGTERM`. Closing the window no longer leaves `.db-wal` / `.db-shm` behind.

---

## Changes by Area

| Area | Before (v0.4.8) | After (v0.5.0) |
|------|-----------------|----------------|
| Frontend | Dear ImGui + GLFW/OpenGL3 window (`src/ui/`) | FTXUI terminal UI (`src/tui/`) |
| Input box | Single line, horizontal scroll on overflow | Soft-wrapped, up to 8 rows, cursor tracks byte offset |
| Reasoning | Expanded, full text inline | Collapsed summary + trailing window (F8) |
| Chat scroll | Fixed row index with a 1e9 bottom sentinel | Clamped to real content/viewport rows |
| Status bar | Plain dim text | Per-segment theme colors on `menuBarBg` |
| Hint bar | Long `F1 about · F2 config · ...` text | Compact keycap chips |
| Table header | `mdTableHdr` used as text color, no background | `headerBg` background + `text` color |
| Table border | `border` color | `headerActive` (higher contrast) |
| Exit handling | None (relied on destructor) | Cross-platform exit handler closes DB |

---

## Bug Fixes

| Issue | Fix |
|-------|-----|
| ↑/↓ and PgUp/PgDn stopped scrolling near top/bottom | Scroll position clamps to `contentRows - viewportRows - 1` instead of a huge sentinel |
| Status bar lost per-segment theme colors | `status_bar::renderSegments` + theme field mapping |
| Table header text invisible in light themes | Header uses `text` color + `headerBg` background |
| `.db-wal` / `.db-shm` left behind on window close | Exit handler checkpoints and closes the DB |
| F8 only collapsed the last reasoning message | Global reasoning collapse state |
| Reasoning flushed the chat with tens of thousands of chars | Trailing window (last 18 lines) + collapsed summary |
| Long user/tool/system text clipped at the edge | `renderPlainText` soft-wrap |

---

## Known Issues

| # | Issue | Impact |
|---|-------|--------|
| — | Input cursor maps to the byte offset exactly; vertical (↑/↓) cursor movement inside the input is not implemented | Edit with ←/→ only |
| — | `python_tool_manager.cpp` Python auto-detection still uses `_popen`/`popen` | Cosmetic — the real pytool path goes through `IProcessRunner` |

---

## File Changelog

### New
| File | Description |
|------|-------------|
| `src/tui/main_tui.cpp` | Entry point + FTXUI event loop + app wiring |
| `src/tui/app_tui.cpp/.h` | TUI application shell (backend wiring) |
| `src/tui/chat_view.cpp/.h` | Chat bubble rendering (roles, reasoning, tool blocks) |
| `src/tui/markdown_view.cpp/.h` | Markdown + plain-text soft-wrap renderer |
| `src/tui/markdown_text.cpp/.h` | UI-agnostic markdown parser |
| `src/tui/bubble_model.cpp/.h` | Message → display bubble pure logic |
| `src/tui/status_bar.cpp/.h` | Status bar segments |
| `src/tui/status_line.cpp/.h` | Live agent phase line |
| `src/tui/config_view.cpp/.h` | Config / welcome / approval dialogs |
| `src/tui/theme_*.cpp/.h` | Theme model, manager, editor, color mapping |
| `src/tui/todo_view.cpp/.h` | TODO panel |
| `src/tui/terminal.cpp/.h` | Terminal init |
| `tests/golden/`, `tests/unit/` | doctest unit + FTXUI golden tests |
| `external/FTXUI-7.0.3/` | FTXUI terminal UI framework (static) |
| `doc/ftxui_migration_plan.md` | Migration execution plan |
| `doc/tui_parity_gap.md` | ImGui↔FTXUI behavior gap checklist |

### Modified
| File | Key Changes |
|------|-------------|
| `CMakeLists.txt` | FTXUI targets, tests, platform-aware sources |
| `README.md` / `README_zh.md` | TUI rewrite, shortcuts, architecture |
| `src/platform/isystem_util.h` | Added `InstallExitHandler` |
| `src/platform/system_util_win.cpp` | `SetConsoleCtrlHandler` for window close / Ctrl+C |
| `src/platform/system_util_linux.cpp` | `sigaction` for SIGHUP/SIGINT/SIGTERM |
| `src/core/agent.cpp` | User message persistence moved to `startTurn`; `/save` `/load` removed |
| `src/core/storage.cpp` | WAL checkpoint on close |
| `src/client/models.h` | Default context window raised to 1M |

### Removed
| File | Reason |
|------|--------|
| `src/ui/` (app, render_chat, render_settings, markdown_render, theme, theme_popup) | Replaced by `src/tui/` |
| `src/gui_backend.cpp/.h` | GLFW/OpenGL backend no longer used |
| `external/glfw/` | No longer used |
| `assets/*.png` GUI screenshots | Stale ImGui screenshots |
| `assets/msyh.ttc` | Bundled font moved to runtime `projv_files/fonts/` |
