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

## Reading the screen

| Element | Meaning |
|---------|---------|
| spinner + "Thinking..." | request in flight |
| collapsed reasoning line | press `F8` for the full thinking chain |
| `[Role]` bubbles | multi-agent skill output |
| agent sidebar | per-role status, message count and live activity |

---

Next -> [06 skills](06-skills.md) | Want to change it -> [storage / config](../dev-guide/module-map.md)
