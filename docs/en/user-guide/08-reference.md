# 08 - Reference

> Every shortcut, `/` command and config key, in one place.

## Keyboard

| Key | Action |
|-----|--------|
| `F1` | About |
| `F2` | Configuration |
| `F3` | New chat |
| `F4` | MCP servers (toggle / restart) |
| `F5` | Open session (picker over `projv_files/sessions/`) |
| `F6` | Switch theme |
| `F7` | Theme editor |
| `F8` | Toggle reasoning (collapsed summary <-> full chain) |
| `F9` | Toggle TODO panel |
| `F10` | Switch model (fetched from the API) |
| `Tab` | Cycle role |
| `Up` / `Down` | Scroll chat by one row |
| `PgUp` / `PgDn` | Scroll chat by one page |
| `Enter` | Send |
| `Esc` | Cancel the running turn / quit when idle |
| `Ctrl+C` | Quit (session is checkpointed and closed) |

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
| `@<path>` | Attach images (Flash models only) |

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
| `exec_shell` | Run a command (approval required, 30 min cap) |
| `web_search` | DeepSeek Search API |
| `fetch_url` | DeepSeek Fetch API (returns Markdown) |
| `update_todo` | TODO panel: `init` / `update` / `done` |
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
| `max_tokens` | reply budget (default 65536) |
| `temperature` | 0.0 by default (no effect in thinking mode, per the API docs) |
| `reasoning_effort` | thinking depth: `none` / `low` / `high` / `max`, default `max`; `none` turns thinking off (the API default level is `high`) |
| `context_window` | 0 = auto |
| `theme` | active theme name |
| `osc_status` | `true` (default): report `working` / `blocked` / `done` / `error` to the hosting terminal over OSC 7501; hosts that do not know it ignore it, and a redirected stdout gets no extra bytes. A normal exit leaves `clear` behind; an interrupted exit (Ctrl-C, window close) leaves `error` so the run stays visible. `PROJV_NO_OSC=1` forces it off |
| `cpp_compiler_path` | optional, for build tasks |
| `python_path` | interpreter for pytool; empty = `python` |
| `clang_format_path` | binary used by `format_file`; empty = PATH |
| `[model_prices]` | prefix -> input/output price per 1M tokens, for the cost estimate |
| `snapshots_max_workspace_gb` | default 2. A workspace bigger than this disables `/undo` (the gate exists so a huge tree is not copied every turn); `0` turns the gate off |
| `snapshots_keep_days` | default 7. Snapshots older than this are pruned at startup |

## Exit codes / limits

| Limit | Value |
|-------|-------|
| Shell / pytool timeout | 30 minutes, one process at a time |
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
| A dialog appeared before a shell command | That command deletes or overwrites something (`rm`/`del`, `mv`/`cp` onto an existing file, a `>` redirect, `git clean`/`reset --hard`, `python -c`). "Execute once" runs it now; "Always" allows that exact command text for the rest of the session; "No" drops the whole batch and tells the model. |
| `blocked by policy: ... cannot be undone` | Disk formatting or partitioning (`format`, `diskpart`, `mkfs`, `fdisk`, `parted`, `dd of=/dev/...`). There is no dialog and no override: run it yourself if you really mean it. |
| Where are the logs? | `projv_files/logs/proJV-YYYY-MM-DD.log`, one file per day, 7 days kept. Crashes add `crash-<timestamp>.log` and `.dmp` in the same folder. A release build writes WARNING and ERROR only; a debug build writes everything. |

---

Next -> back to [the guide](README.md) | Want to change it -> [dev guide](../dev-guide/README.md)
