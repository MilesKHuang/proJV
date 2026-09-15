# proJV v0.5.2 Release Notes

> Date: 2026-09-15
> Compare: `v0.5.1` -> `v0.5.2`

---

## Highlights

### Context compression fixed

`/compress` now runs on the agent thread and produces a real LLM summary
instead of truncating text by bytes. The compaction plan preserves tool-call
pairs, keeps the system prompt, feeds the previous summary back into the next
summary (so it stays exactly one summary), and avoids HTTP 400 after
compaction.

### exec_shell timeout is explicit and unified

`exec_shell` accepts an optional `timeout_ms` argument:

- Default: 300000 ms (5 minutes)
- Cap: 600000 ms (10 minutes)

The old "kill after 30 seconds without output" logic is gone, so quiet
commands such as downloads, builds, and `git clone` are no longer killed
prematurely. The same default timeout now also applies to Python tools
(`pytool`).

### Logs go to a file instead of corrupting the TUI

Debug logs are written to `proJV.log` next to the executable and stderr
logging is disabled, so log output no longer overwrites the FTXUI screen.

### Dead code removed

Removed `Session::serialize()` / `Session::deserialize()` and
`shell_tool.cpp`'s unused `isPathWithinWorkspace()`.

---

## Changes by Area

| Area | Before (v0.5.1) | After (v0.5.2) |
|------|-----------------|----------------|
| `/compress` | Byte-truncation on UI thread | Agent-thread LLM summary, tool-pair safe, single summary |
| `exec_shell` timeout | No total limit, 30s idle kill | Optional `timeout_ms`, 5 min default, 10 min cap |
| `pytool` timeout | No total limit | 5 min default |
| Debug logging | stderr, overwrites TUI | `proJV.log` file, stderr silenced |

---

## File Changelog

### Modified

| File | Key Changes |
|------|-------------|
| `src/core/session.cpp/.h` | Fixed compaction plan, summary dedup, pruning; removed JSON persistence |
| `src/core/agent.cpp/.h` | `/compress` via agent thread, forced compaction, two-level `doCompaction` |
| `src/tui/app_tui.cpp` | `/compress` routes through agent thread |
| `src/client/deepseek.cpp` | Reset cancel flag before non-streaming compaction request |
| `src/platform/iprocess_runner.h` | Added `idleTimeoutMs`, shared timeout constants |
| `src/platform/process_runner_win.cpp` | Removed 30s idle kill |
| `src/platform/process_runner_linux.cpp` | Removed 30s idle kill |
| `src/tools/shell_tool.cpp` | `timeout_ms` param, timeout result handling |
| `src/tools/python_tool_manager.cpp` | Default 5 min subprocess timeout |
| `src/core/prompts_loader.cpp` | Updated default coder prompt with `timeout_ms` note |
| `src/tui/main_tui.cpp` | File logging, About version bump |

### Removed

| File | Reason |
|------|--------|
| `RELEASE_NOTES_v0.5.1.md` | Superseded by v0.5.2 |
