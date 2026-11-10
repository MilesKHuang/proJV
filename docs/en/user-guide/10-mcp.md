# 10 - MCP servers

> [中文](../../zh/user-guide/10-mcp.md) | **English**

proJV can talk to [MCP](https://modelcontextprotocol.io) servers. An MCP server is
an ordinary external process that speaks JSON-RPC over stdin/stdout; proJV starts
it, keeps it alive across turns, and turns each of its tools into a tool the model
can call.

Nothing is loaded as code. If you can run it from a terminal, you can plug it in.

## 1. Declare a server

The list lives in `projv_files/mcp.json`, next to the executable (same folder as
`config.toml`). It is created empty on first launch:

```json
{
  "servers": []
}
```

Add one entry:

```json
{
  "servers": [
    {
      "name": "time",
      "description": "Clock, timezone conversion and timers",
      "command": "uvx",
      "args": ["mcp-server-time"],
      "protocol": "stateful",
      "trusted": true,
      "enabled": true,
      "timeout_ms": 120000
    }
  ]
}
```

| Field | Required | Meaning |
|-------|----------|---------|
| `name` | yes | Unique. Becomes the tool prefix: `mcp__time__get_current_time` |
| `description` | yes in practice | One line on what the server is for. It is the only source of that sentence in the capability map the model reads every turn; without it the model sees a server name and has to guess |
| `command` | yes | Executable, resolved on `PATH`. No shell is involved |
| `args` | no | Argument array. One array element = one argument |
| `env` | no | Extra environment variables for the child |
| `protocol` | no | `"stateful"` (default, legacy `initialize` handshake) or `"stateless"` (modern: no handshake, every request carries its own metadata) |
| `trusted` | no | **`false` by default. A server that is not trusted is never started** |
| `enabled` | no | `false` by default. Must be turned on explicitly, or with `F4` |
| `timeout_ms` | no | Per-request timeout, default `120000` |

A bad entry (missing `name`, unknown `protocol`, duplicate `name`) is skipped with
a log line; it does not take the rest of the list down. A missing `description` is
kept but logged -- the model will not know what that server is for.

## 2. When an edit takes effect

**Editing `mcp.json` takes effect on your next message. No restart.**

Before each turn, on the UI thread and only while nothing is running, proJV
compares the file's timestamp and size against the last load. If they moved, it
re-reads the list and diffs it by name:

| Change | What happens |
|--------|--------------|
| New entry | Added; started only when `enabled` and `trusted` |
| Deleted entry | Process stopped at once, tools dropped at the start of the next turn |
| Same entry, `enabled` flipped | Started or stopped; the process is not respawned |
| Same entry, `command`/`args`/`env`/`protocol`/`trusted`/`timeout_ms` changed | Restarted with the new definition |
| Only `description` changed | Nothing restarted; the capability map picks up the new text |
| A server that is already `dead` | **Not retried automatically** -- `R` in `F4` is the retry button, otherwise every message would retry a broken server |
| Invalid JSON (half-saved file) | The running servers are left completely untouched; the parse error is shown in `F4` |

`F4` is still the fastest way to toggle one server, and it writes `enabled` back to
the file.

## 3. Turn it on

Two ways, both equivalent:

- set `"enabled": true` and send a message (or restart proJV), or
- press `F4`, select the server, press `Enter`.

`F4` also shows the live state (`stopped` / `connecting` / `handshaking` /
`ready` / `dead`), the number of tools each server contributed, and the selected
server's `description`. `R` restarts the selected server. Toggling writes `enabled`
back to `mcp.json`, so the choice survives a restart.

**The toggle is refused while a turn is running.** Starting or stopping a server
changes the tool table, and that is only legal between turns. Finish the turn (or
press `Esc`) first.

## 4. Use the tools

Once a server is `ready`, its tools appear as `mcp__<server>__<tool>` and are
visible to the model **without adding anything to your role prompt** -- the names
only exist at runtime, so there is nothing to pre-write.

You do not normally need to name a tool. Ask for the outcome:

```
what time is it in Tokyo?
```

To check the wiring by hand, without starting the TUI:

```
proJV_tui --mcp-check
proJV_tui --mcp-call time get_current_time timezone=Asia/Tokyo
```

`--mcp-check` prints every configured server, its state and its tools, and exits
non-zero if an enabled + trusted server failed. `--mcp-call` additionally performs
one call. Arguments are `key=value` pairs; a value that parses as JSON is passed
as that type, otherwise as a string.

## 5. Security

`"trusted": false` is the only switch. It is checked before anything is spawned:
an untrusted server does not run, does not register tools, and costs nothing. There
is no per-tool approval prompt -- once a server is trusted and enabled, the model
may call any tool it exposes.

Treat `mcp.json` like `config.toml`: it can hold a token in `env`, and it is
gitignored. A server that writes files acts with your user's permissions.

## 6. Two things that surprise people

**The process runs in the directory proJV was started in.** Changing the workspace
requires restarting proJV. A server that takes a directory argument (`"."`) is
pointing at the launch directory.

**The `enabled` flag is global, not per project.** `mcp.json` sits next to the
executable, so turning a server off in one project turns it off everywhere.

**The first start of a `uvx`/`npx` server is slow.** The launcher downloads the
package first -- tens of megabytes, well past a 120s timeout on a cold cache. Warm
it once yourself:

```
uvx mcp-server-time < nul        (Windows)
uvx mcp-server-time < /dev/null  (Linux)
```

or raise `timeout_ms`. The symptom of skipping this is a server stuck in
`handshaking` and then `dead`.

## 7. What this version does not do

- **stdio only.** No HTTP / SSE transport.
- **No `tools/list` pagination.** A server that paginates is refused as a whole,
  deliberately: registering half its tools would make the model guess at names.
- **No resources or prompts methods.** Tools only.
- **No multi-round input requests.** A server answering `input_required` shows up
  as a clear tool error.
- **Skills never see MCP tools.** Skill subagents run concurrently and would share
  one server's stdin; `mcp__*` names in a skill's `tools` list are skipped at load.

## 8. Troubleshooting

| Symptom | Cause |
|---------|-------|
| state stays `stopped`, tools `0` | `trusted` or `enabled` is false, or the name was skipped as a duplicate |
| state `dead`, error mentions `command not found` | `command` is not on `PATH` for the process that launched proJV |
| state `dead`, error mentions `protocol version` | the server rejected the handshake; check `protocol` (`stateful` vs `stateless`) |
| stuck in `handshaking`, then timeout | the launcher was still downloading the package -- see section 5 |
| tools listed but the model never calls them | the server is `ready` but the turn started before it finished -- the next turn sees them |
