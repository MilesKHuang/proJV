# 05 - Daily use

> After this page you can manage sessions, watch the TODO panel and re-theme
> the app.

## Sessions

Every conversation is a SQLite database in `projv_files/sessions/`, written as
you type.

| Action | How |
|-------|-----|
| new chat | `F3` |
| open an old one | `F5` (picker) |
| clear the current one | `/clear` |
| shrink a long one | `/compress` - summarises early messages and keeps recent ones |

Sessions survive crashes: pages are checkpointed, and a `recovery.db` is used
when a write was interrupted.

## AGENTS.md

Put an `AGENTS.md` in the **workspace root** and proJV reads it into every new
conversation, wrapped in `[AGENTS.MD] ... [END AGENTS.MD]`. It is the place for
what you cannot infer from the code: build and test commands, house rules,
things the agent must never touch.

- Only the root file is read: sub-directories are not scanned, and there is no
  `CLAUDE.md` fallback.
- Editing it takes effect in the **next** conversation (`F3`) or after `/clear`,
  because the block is written into the session at its start.
- The content is capped at 16 KiB; a longer file is truncated and says so.
- Skills do not receive it: their role prompts are already mandatory reading, and
  ten concurrent workers would each pay for another copy.

## TODO panel

```text
update_todo(action="init" | "update" | "done", tasks="1. ...", brief="...")
```

The panel docks on the right (`F9` toggles it). When a task finishes it records
a one-line brief, so the panel doubles as a log of what the agent actually did.
Long entries soft-wrap; nothing is clipped.

## Themes

- `F6` switches theme.
- `F7` opens the visual editor: every colour is editable with a live preview,
  and themes import/export as JSON.
- 2 built-in (`Obsidian`, `Light`) + 6 files in `projv_files/theme/`
  (`artism_warm`, `cyber_punk`, `forest`, `vibrant_focus`, `colorblind_safe`,
  `monochrome_dark`).
- Your choice is saved to `config.toml` and restored on the next launch.

## Status bar

Shows model, role, token counts, a cost estimate (from `[model_prices]` in
`config.toml`) and context pressure. When pressure gets high, proJV compacts
using the `compactor` prompt.

## Diff bubbles

`write_file`, `edit_file`, `md_file` and `format_file` show what they changed: a
`Diff` block appears under the tool call. When the chat column is at least 100
cells wide it is a two-column patch - old line numbers and text on the left, new
ones on the right, a changed line paired with its replacement on the same row.
Narrower terminals get the single-column unified diff (`+` added, `-` removed,
`@` hunk header).

- At most 40 rows per block; the full patch stays in the session database.
- Unchanged context is 2 lines per side, so a screen holds more than one hunk.
- `format_file` on an already-formatted file shows no block at all.
- The `F9` TODO dock takes 36 cells; with it open the chat column may drop below
  the split threshold and the diff falls back to one column.
- The diff is display-only. It is stored with the tool row, so `F5` replays it,
  but it is never sent to the model and costs no tokens.

## Reading the screen

| Element | Meaning |
|---------|---------|
| spinner + "Thinking..." | request in flight |
| collapsed reasoning line | press `F8` for the full thinking chain |
| `[Role]` bubbles | multi-agent skill output |
| `── Diff ──` block | what the last file write changed |
| agent sidebar | per-role status, message count and live activity |

---

Next -> [06 skills](06-skills.md) | Want to change it -> [storage / config](../dev-guide/module-map.md)
