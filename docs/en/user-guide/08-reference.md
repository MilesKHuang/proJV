# 08 - Reference

> Every shortcut, `/` command and config key, in one place.

## Keyboard

| Key | Action |
|-----|--------|
| `F1` | Help: every command and key, scrollable |
| `F2` | Configuration |
| `F3` | New chat |
| `F4` | Open session (picker over `projv_files/sessions/`) |
| `F5` | MCP servers (toggle / restart) |
| `F6` | Switch theme |
| `F7` | Theme editor |
| `F8` | Reveal / collapse the reasoning chain |
| `F9` | Toggle the dock (TODO + background tasks) |
| `F10` | Switch model (fetched from the API) |
| `Tab` | Cycle role |
| `Up` / `Down` | Scroll chat by one row |
| `PgUp` / `PgDn` | Scroll chat by one page |
| `Enter` | Send |
| `Esc` | Cancel the running turn / clear the input / ask before quitting |
| `Ctrl+C` | Nothing: the terminal keeps it for copying a selection |

While a turn is running, the keys that change the session do nothing and put one
line in the chat: `busy -- finish the current turn first`. Those keys are `F3` (new
chat), the `Enter` of the `F4` session picker, the `Enter` of the `F10` model
picker, `Tab` (role), and saving the `F2` dialog. Opening a picker (`F4`, `F10`) is
read-only and still allowed; only the action is held back. `F5` opens and lists as
usual, but while a turn runs its panel shows that same sentence and neither `Enter`
nor `R` acts on a server. The display-only keys (`F1`, `F6`-`F9`) stay live. The
window (studio) refuses the same actions and puts the same sentence in its status
line.

## Commands

| Command | Action |
|---------|--------|
| `/help` | In-chat help |
| `/clear` | Clear the current session |
| `/compress` | Summarise early messages |
| `/undo` | Undo the file changes of the last turn. `/undo!` also discards edits made by hand after that turn |
| `/restore N` | Roll the workspace back to before turn N; needs `/restore N confirm` |
| `/workspace` | Print the current workspace |
| `/skill list` | List installed skills |
| `/<skill> <topic>` | Run an installed skill, e.g. `/monica D:\proj` |
| `/wish <what you want>` | Wish mode: pick the open decisions yourself and keep working until the TODO list is empty. `/wish` alone shows progress, `/wish cancel` stops it and keeps the TODO list |
| `/end_task <id>` / `/end_task all` | Stop a background task (process tree included) |
| `/approval` | Show the approval mode and the commands remembered forever; `/approval clear` forgets them |
| `@<path>` | Attach images (Flash models only) |

Typing `/` in the input box opens the command list above it: keep typing to filter,
`Up`/`Down` to move, `Enter` to fill the command into the input (it is not sent),
`Esc` to close. A space after the command name closes the list, so `/monica ` never
covers the topic you are typing.

## Built-in tools

| Tool | Does |
|------|------|
| `read_file` | Read a file. `start_line`/`end_line` for large files (1000 lines / 100KB per slice) |
| `write_file` | Write a small file (8KB cap, `mode="append"` to extend) |
| `edit_file` | Replace an exact text span - the safe way to edit |
| `md_file` | Write/patch a `.md` file (templates, section replace) |
| `grep_files` | Regex search over the workspace; `max_results` up to 500 |
| `file_search` | Find files by name |
| `list_dir` | Directory tree with `depth` and `exclude` |
| `format_file` | clang-format, or a whitespace-only fallback |
| `exec_shell` | Run a command (approval required, 30 min cap). Long output keeps the **last** 400 lines (`tail_lines` 10-2000, or `full_output` for up to 200 KB). `background: true` detaches it as `bg#N`; when it ends the model is told and carries the work on by itself, even if the window was idle |
| `web_search` | DeepSeek Search API |
| `fetch_url` | DeepSeek Fetch API (returns Markdown) |
| `update_todo` | TODO panel: `init` / `update` / `done` |
| `wish_decide` | Wish mode only: record a decision taken on your behalf |
| `wish_block` | Wish mode only: give up and say what is missing |
| `task_output` | Read a background task's output (same tail-first shaping as `exec_shell`) |
| `run_skill` | Start an installed skill from inside a turn |
| `view_image` | Look at images on disk; they arrive in the next turn as a user message (Flash models only) |

`diagram_tool` is not built in: it is the example pytool in
`projv_files/pytool/diagram_tool/` (it needs Graphviz).

## config.toml

| Key | Meaning |
|-----|---------|
| `api_key` | DeepSeek key |
| `model` | `deepseek-v4-flash` (default) or `deepseek-v4-pro` |
| `base_url` | API endpoint, default `https://api.deepseek.com` |
| `max_tokens` | reply budget (default 65536). Values outside [1024, 65536] are clamped at load with a warning; a retry after `finish_reason=length` never lowers it |
| `temperature` | 0.0 by default (no effect in thinking mode, per the API docs) |
| `reasoning_effort` | thinking depth: `none` / `low` / `high` / `max`, default `max`; `none` turns thinking off (the API default level is `high`) |
| `context_window` | 0 = auto |
| `theme` | active theme name |
| `osc_status` | `true` (default): report `working` / `blocked` / `done` / `error` to the hosting terminal over OSC 7501; hosts that do not know it ignore it, and a redirected stdout gets no extra bytes. A normal exit leaves `clear` behind; an interrupted exit (window close) leaves `error` so the run stays visible. `PROJV_NO_OSC=1` forces it off |
| `cpp_compiler_path` | optional, for build tasks |
| `python_path` | interpreter for pytool; empty = `python` |
| `clang_format_path` | binary used by `format_file`; empty = PATH |
| `[model_prices]` | prefix -> input/output price per 1M tokens, for the cost estimate |
| `snapshots_max_workspace_gb` | default 2. A workspace bigger than this disables `/undo` (the gate exists so a huge tree is not copied every turn); `0` turns the gate off |
| `snapshots_keep_days` | default 7. Snapshots older than this are pruned at startup |
| `approval_mode` | `strict` / `balanced` (default) / `loose`. **strict**: every delete, overwrite, redirect and `git commit`/`push` asks, and there is no ALWAYS. **balanced**: destructive commands ask, build output such as `build > log.txt` runs. **loose**: only formatting/partitioning is refused. The old `auto_approve` key is retired and still read as `true` -> balanced, `false` -> strict |
| `approval_grants` | commands you allowed forever with ALWAYS (exact text). Written by the dialog, listed and cleared by `/approval`, kept across restarts and across `/clear` |

`F2` edits every key above except `[model_prices]`: API key, base URL, model,
`max_tokens`, temperature, `reasoning_effort`, `context_window`, the three
toolchain paths, the two snapshot keys, theme, `osc_status`, the approval mode and
the remembered-command list (with a Clear button). Values are clamped to the same
ranges the loader enforces, so the dialog can never write a value the loader would
reject.

## Exit codes / limits

| Limit | Value |
|-------|-------|
| Shell / pytool timeout | 30 minutes, one process at a time |
| Background task timeout | 60 minutes, then killed and marked `timed out` |
| Background output kept | last 100 KB per task (temp file, removed at exit) |
| `write_file` chunk | 8 KB |
| `read_file` whole file | 100 KB (use line ranges above that) |
| `grep_files` results | 100 by default, 500 max |
| Images per message | 8 |

## Troubleshooting

| Symptom | Cause |
|---------|-------|
| `outside the workspace` | the path is not under the launch directory (the workspace) |
| tools "not allowed" in a skill | the skill's `tools` list does not include them |
| `cannot dispatch to a driver agent` | dispatch is one layer deep by design |
| `[skill] config: unknown key 'x'` | typo in `config.json`; the value was ignored |
| `format_file` says not found | install `clang-format` or set `clang_format_path`. Visual Studio ships one: `...\VC\Tools\Llvm\x64\bin\clang-format.exe` |
| `TLS certificate verification failed` | proJV verifies the server certificate and has **no switch to turn that off**. Behind a proxy or gateway that re-signs TLS, install that proxy's root CA in the OS trust store (Windows: Trusted Root Certification Authorities; Linux: `/usr/local/share/ca-certificates` + `update-ca-certificates`). Also check the system clock: a wrong date fails the same way. |
| A dialog appeared before a shell command | That command deletes or overwrites real data (`rm`/`del`, `mv`/`cp` onto an existing file, `git clean`/`reset --hard`/`commit`/`push`, inline `python -c` / `node -e` code that removes or writes files). Under `approval_mode = balanced` redirection (`> log.txt`) and read-only one-liners (`py -3 -c "import sqlite3; ..."`) do not ask. "Execute once" runs it now; "Always" runs it now **and** remembers that exact command in `config.toml`; "No" drops the whole batch and tells the model. Under `strict` there is no "Always" at all. |
| `blocked by policy: ... cannot be undone` | Disk formatting or partitioning (`format`, `diskpart`, `mkfs`, `fdisk`, `parted`, `dd of=/dev/...`). There is no dialog and no override: run it yourself if you really mean it. |
| Where are the logs? | `projv_files/logs/proJV-YYYY-MM-DD.log`, one file per day, 7 days kept. Crashes add `crash-<timestamp>.log` and `.dmp` in the same folder. A release build writes WARNING and ERROR only; a debug build writes everything. |

---

## Headless: `projv run`

One prompt, no TUI, one event per line. Same agent as the interface, so a script
gets exactly what you would have seen.

```
projv run -p "say hi" [--jsonl] [--no-ts] [--max-turns N]
                       [--session PATH] [--approve ask|never|always]
                       [--dump-request] [--schema]
```

| Flag | Meaning |
|---|---|
| `-p, --prompt` | the line to send (required unless `--schema`) |
| `--jsonl` | emit the event stream (one JSON object per line) instead of text |
| `--no-ts` | drop timestamps, so two runs of the same prompt are byte-identical |
| `--max-turns` | how many user submissions to allow (default 1) |
| `--session` | open an existing session database instead of a new one |
| `--approve` | `ask` (default; refused here, there is no dialog), `never`, `always` |
| `--dump-request` | print the first request body to stderr, for diffing |
| `--schema` | print the event schema and exit |

Exit codes: `0` ok, `1` error, `2` cancelled, `3` an approval was needed but
`--approve never` refused it. `projv run --schema` lists every event and its
fields; `projv run -p "..." --jsonl` is what a CI job parses.

## The window: `proJV_studio`

```
proJV_studio [--dump-view-model [file]] [--screenshot out.png]
             [--frames N] [--expand KEY] [--scroll-up N]
             [--demo-chat [--demo-stream N]]
```

| Flag | Meaning |
|---|---|
| `--dump-view-model [file]` | write one frame's view-model JSON and exit (the parity oracle) |
| `--screenshot out.png` | render, read the back buffer, write a PNG, exit |
| `--frames N` | how many frames to render before the screenshot (default 4) |
| `--demo-chat` | fill a fixed transcript, so no model or API key is needed |
| `--demo-stream N` | append a demo bubble every N frames (follows the end like a live turn) |
| `--expand KEY` | open one folded row by key (e.g. `m10:0`) before the shot |
| `--scroll-up N` | scroll back N lines before the shot |

`--dump-view-model` and `--screenshot` are the two acceptance hooks: the first says
what the derivation produced, the second says what a reader actually sees. Neither
opens a dialog, and `--demo-chat` makes both work on a machine with no key. The
window's own behaviour (fold, follow, layout) is described in
[the studio guide](../dev-guide/studio-gui.md).

Selecting text with the mouse and `Ctrl+C` copy exactly the glyphs under the
highlight, Chinese included: the hit-test, the band and the clipboard all read the
per-character pixel offsets the font measured, so there is no "columns x one
character width" guess left to drift. Two consequences worth knowing: a table is
selected and copied as one block (its cells come back tab-separated), and a folded
tool row is not on screen, so a selection that spans it skips its body.

---

Next -> back to [the guide](README.md) | Want to change it -> [dev guide](../dev-guide/README.md)
