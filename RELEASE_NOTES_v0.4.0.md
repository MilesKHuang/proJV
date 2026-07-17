# proJV v0.4.0 Release Notes

> Date: 2026-07-18
> Compare: `v0.3.x` (master) → `v0.4.0`

---

## Highlights

### Designer Workflow (New)
- **`md_file` tool**: AI writes/edit design documents as `.md` files. Write capped at 8KB, edit via SEARCH/REPLACE. `.md` suffix enforced, workspace-bound.
- **`designer` role**: Hardcoded `DESIGNER_PROMPT_DEFAULT` preset. Tools limited to `md_file, read_file, grep_files` -- no shell, no source edits.
- **Directory-driven prompts**: `projv_prompts/*.md` auto-scanned. Three presets: `coder.md`, `designer.md`, `compactor.md`. Drop a custom `.md` in and it auto-appears in the UI.

### Role Switching via ComboBox
- **ComboBox** in the input area footer, left of the status line. Always visible, greyed-out when agent is busy.
- **Switch mid-conversation without clearing context**. `Agent::replaceSystemPrompt()` swaps only the first system message in session; all other messages preserved.
- **`compactor.md` hidden** from ComboBox (still active via `/compress`).

### System Prompt Editor Removed
- Old `Settings → System Prompt` menu item and `renderSystemPromptPopup()` deleted. Prompt files are plain `.md` -- edit externally.

### Custom Markdown Renderer
- Replaced `imgui_markdown` (md4c-based) with a custom `markdown_render.cpp`. Handles headings, bold, code blocks, tables, links, and bullet lists. No more md4c crashes on malformed markdown.

---

## Architecture Changes

| Area | Before (master) | After (v0.4.0) |
|------|----------------|----------------|
| Agent model | 3 threads (UI + Agent + WriteQueue) | 2 threads (UI + Agent). `StorageWriteQueue` and `ToolWorker` merged into Agent thread. |
| Prompt loading | `loadSystemPrompt()`, `loadCompactionPrompt()` -- hardcoded single-file | `ensureDefaultPrompts()` + `loadPromptFile(name)` -- directory-driven, user-extensible |
| Tools | No design-doc tool | `md_file` for designer workflow |
| UI | Built-in System Prompt editor popup | ComboBox role switcher; external `.md` editing |

---

## File Changelog

### New Files
| File | Description |
|------|-------------|
| `src/tools/md_file_tool.h` / `.cpp` | `md_file(action, path, ...)` -- write/edit .md design docs |
| `src/ui/markdown_render.cpp` | Custom Markdown renderer (replaces imgui_markdown) |
| `doc/threading_model.md` | Thread model reference doc |
| `doc/design_subagent.md` | Sub-agent design proposal |
| `doc/design_skill.md` | Designer workflow design doc |

### Modified Files
| File | Key Changes |
|------|-------------|
| `src/core/prompts_loader.cpp` | Deleted `loadSystemPrompt()`, `loadCompactionPrompt()`; added `ensureDefaultPrompts()`, `loadPromptFile()`, 3 prompt presets |
| `src/core/prompts.h` | Updated declarations |
| `src/core/agent.h` / `.cpp` | `replaceSystemPrompt()`; compaction now calls `loadPromptFile("compactor.md")`; single-threaded tool execution |
| `src/core/session.cpp` | Compaction logic polish |
| `src/ui/app.h` / `.cpp` | `promptFiles_`, `activePromptIndex_`; deleted `systemPromptBuf`, `showSystemPromptEdit`; `setupTools()` registers `md_file` |
| `src/ui/render_chat.cpp` | ComboBox in `renderInputArea()`; prompt switch logic |
| `src/ui/render_settings.cpp` | Deleted `renderSystemPromptPopup()` |
| `src/tools/file_tool.cpp` | Workspace violation detection improvements |
| `src/client/deepseek.cpp` | HTTP 400 retry, cancel propagation |
| `CMakeLists.txt` | Added `md_file_tool.cpp` |

### Deleted Files
| File | Reason |
|------|--------|
| `src/core/storage_queue.cpp` / `.h` | Merged into Agent thread |
| `src/core/tool_worker.cpp` / `.h` | Merged into Agent thread |
| `external/imgui_markdown.h` | Replaced by custom markdown_render |

---

## Bugfixes
- **md_file edit LF normalization**: Fixed `search.size()` mismatch vs actual CRLF length in file -- `\r\n` caused residual `\n` characters on append (extra blank lines).
- **Designer prompt**: Removed excessive template that caused LLM to insert blank lines inside tables.
- **HTTP 400 recovery**: Compaction is now triggered only for context-length errors, not all 4xx.
- **Destructive command detection**: Expanded keyword coverage for `del`, `rm`, `rmdir`, `rd`, `remove-item`.

---

## Upgrade Notes
- Existing `config.toml` works as-is.
- `projv_prompts/system_prompt.md` renamed to `coder.md` on first launch. Old custom prompts can be moved manually.
- Remove `projv_prompts/system_prompt.md` and `projv_prompts/compaction_prompt.md` if present -- they are auto-migrated.
