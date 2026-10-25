# proJV user guide

proJV is a terminal AI agent written in native C++ (TUI, DeepSeek). Install it,
talk to it, and it can read and write files, run commands, search the web, and
run multi-agent skills.

## Read in this order

| # | Page | You will be able to |
|---|------|---------------------|
| 01 | [install](01-install.md) | Download and run, or build it yourself on Windows / Linux |
| 02 | [first run](02-first-run.md) | Enter a key, send your first message, approve a tool, cancel a run |
| 03 | [roles](03-roles.md) | Switch roles with Tab and write your own role `.md` |
| 04 | [images](04-images.md) | Send an image with `@path` |
| 05 | [daily use](05-daily.md) | Sessions, TODO panel, themes, compaction |
| 06 | [skills](06-skills.md) | Run `bigbang` / `monica`, read the agent sidebar, author your own |
| 07 | [pytool](07-pytool.md) | Write a Python tool the agent can call |
| 08 | [reference](08-reference.md) | Every shortcut, `/` command and `config.toml` key |

## What it needs

- A DeepSeek API key (only DeepSeek is supported right now).
- A terminal. Windows Terminal is recommended on Windows; any UTF-8 terminal
  works on Linux. SSH and tmux are fine.

## Where it keeps things

Everything lives next to the executable, in `projv_files/`:

```
projv_files/
├── config.toml     config (created on first save, holds your API key)
├── prompts/        role prompts (seeded on first run)
├── theme/          theme JSON files
├── pytool/         Python tools
├── skills/         multi-agent skills (bigbang, monica)
└── sessions/       SQLite session databases
```

Copy the executable and `projv_files/` together and it runs; that pair is the
whole installation.

---

Want to go deeper -> [dev guide](../dev-guide/README.md)
