# proJV v0.4.6 Release Notes

> Date: 2026-07-24
> Compare: `v0.4.4` -> `v0.4.6`

---

## Highlights

### Path Unification
All `projv_*` directories consolidated under a single `projv_files/` directory.

| Old | New |
|-----|-----|
| `{exeDir}/config.toml` | `{exeDir}/projv_files/config.toml` |
| `{exeDir}/projv_theme/` | `{exeDir}/projv_files/theme/` |
| `{exeDir}/projv_prompts/` | `{exeDir}/projv_files/prompts/` |
| `{exeDir}/projv_pytool/` | `{exeDir}/projv_files/pytool/` |
| `{exeDir}/projv_sessions/` | `{exeDir}/projv_files/sessions/` |

Six new helpers in `config.h`: `getExeDir()`, `getProjvDir()`, `getThemeDir()`, `getPromptsDir()`, `getPytoolDir()`, `getSessionsDir()`. All modules use these instead of hardcoding paths.

### Role Merge
`supervisor` + `coder` + `tester` merged into a single **coder** role with full 11-tool access (default).

| Before (v0.4.4) | After (v0.4.6) |
|-----------------|----------------|
| 6 roles: supervisor(11), coder(7), tester(6), designer(3), analyzer(5), compactor(0) | 4 roles: **coder**(11, default), designer(3), analyzer(5), compactor(0) |

UI dropdown: `coder -> designer -> analyzer`. `compactor` hidden (internal use).

### Cancel Fix
Two bugs fixed that caused Cancel to silently fail:

1. **Deadlock**: `cancelAll()` killed `cmd.exe` directly, leaving child processes (`cl.exe`, etc.) alive and holding stdout pipe handles. The reader thread blocked forever in `ReadFile()`, and `reader.join()` never returned. Fixed by removing direct `TerminateProcess` — the polling loop now detects the cancel flag and calls `TerminateJobObject` to kill the entire process tree.

2. **Premature output truncation**: `CloseHandle(hStdoutRd)` was called before `reader.join()` for both cancel and normal-exit paths, discarding buffered pipe data. Fixed: close-before-join only on forced kill; normal exit joins first.

### Streaming Resilience
- **Low-speed timeout**: 120s -> 300s (DeepSeek Reasoner can pause >2 min between reasoning and answer)
- **After-content retries**: 1 -> 2 (more chances to recover a partially-complete stream)
- **Reasoning preservation**: when a stream ends with reasoning content but no final answer, reasoning is saved as a message instead of silently discarded

---

## v0.4.4 Features (carried forward)

### PyTool System
Python tools register alongside C++ built-in tools with equal status. `PythonToolManager` auto-scans `projv_files/pytool/` at startup, parses `tool.json` metadata, and registers valid tools.

### diagram_tool
Graphviz DOT renderer with auto-download of portable `dot.exe`. Zero external dependencies beyond `pip install graphviz`.

---

## Architecture Changes

| Area | Before (v0.4.4) | After (v0.4.6) |
|------|----------------|----------------|
| Directory layout | 6 scattered `projv_*` dirs + root `config.toml` | Single `projv_files/` with config, theme, prompts, pytool, sessions |
| Default role | supervisor (11 tools) | coder (11 tools) |
| Role count | 6 (supervisor, coder, tester, designer, analyzer, compactor) | 4 (coder, designer, analyzer, compactor) |
| Cancel | Unreliable — stuck on long shell commands | Reliable — kills entire process tree, returns to Idle in <100ms |
| Stream timeout | 120s low-speed | 300s low-speed + 2 retries after content |

---

## File Changelog (v0.4.4 -> v0.4.6)

### Modified
| File | Key Changes |
|------|-------------|
| `src/core/prompts_loader.cpp` | Removed supervisor/tester presets; coder merged to 11 tools; `promptsDir()` uses `getPromptsDir()` |
| `src/ui/app.cpp` | Default role `coder.md`; workspace fallback -> `getExeDir()`; sessions -> `getSessionsDir()` |
| `src/tools/registry.h` | Added `cancelAll()`, `isCancelled()`, `resetCancel()`; `activeProcessHandle_` member |
| `src/tools/registry.cpp` | `cancelAll()` sets flag only (no direct `TerminateProcess`) |
| `src/tools/shell_tool.cpp` | Lambda captures `&registry`; 100ms polling + cancel check + `TerminateJobObject`; idle timeout uses absolute timestamps; `CloseHandle` ordering: forced-kill closes before join, normal exit after |
| `src/core/agent.cpp` | `cancel()` calls `toolRegistry.cancelAll()`; `newTurn()` calls `resetCancel()`; ExecutingTools detects `isCancelled`; saves reasoning on empty-content finish |
| `src/client/models.h` | `ToolResult::isCancelled` field |
| `src/core/config.h` | 6 new path helpers: `getExeDir()`, `getProjvDir()`, `getThemeDir()`, `getPromptsDir()`, `getPytoolDir()`, `getSessionsDir()` |
| `src/core/config.cpp` | `getConfigPath()` -> `projv_files/config.toml`; implemented 6 path helpers |
| `src/tools/python_tool_manager.cpp` | `pytoolDir()` uses `getPytoolDir()` |
| `src/ui/theme.cpp` | `init()` receives theme dir directly (no longer appends `/projv_theme`) |
| `src/main.cpp` | `ThemeManager::init(getThemeDir(), ...)` |
| `src/ui/render_chat.cpp` | Workspace fallback uses `getExeDir()` |
| `src/client/deepseek.cpp` | Low-speed timeout 120s -> 300s; after-content retries 1 -> 2 |
| `CMakeLists.txt` | POST_BUILD copies `projv_files/` |

### Deleted
| File | Reason |
|------|--------|
| `projv_pytool/` (moved) | -> `projv_files/pytool/` |
| `projv_theme/` (moved) | -> `projv_files/theme/` |
| `doc/design_multi_role_prompts.md` | Superseded by `design_v0.4.6.md` |
| `doc/design_pytool.md` | Superseded |
| `doc/design_subagent.md` | Superseded |
| `doc/design_theme_system.md` | Superseded |
| `RELEASE_NOTES_v0.4.4.md` | Renamed to this file |

---

## Known Issues
- **migrateLegacyPaths()** not yet implemented: users upgrading from v0.4.4 must manually move `config.toml` and old `projv_*` dirs into `projv_files/`.
- `forcedKill` message always says "timed out after 30 seconds" regardless of actual kill reason (cancel vs idle timeout). Cosmetic only.
