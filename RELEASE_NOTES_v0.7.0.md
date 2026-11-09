# v0.7.0 - MCP servers, and one race closed

## MCP servers

proJV can now drive MCP servers. A server is an ordinary external process
speaking JSON-RPC over stdin/stdout: you declare it in `projv_files/mcp.json`,
proJV spawns it once, keeps it alive across turns, and exposes each of its tools
as `mcp__<server>__<tool>`.

- **Nothing is loaded as code.** Still no plugin registry, no package manager, no
  dynamic library. `mcp.json` is a list of commands, and the old rule still holds:
  an extension is a file.
- **Two protocol eras.** `"protocol": "stateful"` (the default) is the legacy
  `initialize` handshake and works with the servers that exist today.
  `"protocol": "stateless"` is the modern, handshake-free form where every request
  carries its own metadata.
- **An arbitrary JSON Schema survives the trip.** A server's own `inputSchema` is
  forwarded byte for byte, so nested objects, `$ref`, `$defs`, `anyOf` and `enum`
  reach the model intact. The flat four-type tool schema is unchanged.
- **New tool names are visible without editing a prompt.** MCP names only exist at
  runtime, so they reach the model through a second channel that is orthogonal to
  the `tools available:` line. Deleting that line still means "no restriction",
  exactly as before.
- **`F4` is the panel.** Toggle a server, restart it, and read its protocol, live
  state and how many tools it contributed. Toggling writes `enabled` back to
  `mcp.json`, so the choice survives a restart. It refuses while a turn is
  running, because starting or stopping a server rewrites the tool table.
- **`trusted` is the only security switch, and it fails closed.** An untrusted
  server is never spawned, so it costs nothing to keep one in the file.
- **Headless checks.** `proJV_tui --mcp-check`, and
  `proJV_tui --mcp-call <server> <tool> [key=value ...]`, verify a connection
  without starting the TUI.

Scope is deliberately narrow: stdio only; no `tools/list` pagination (a paginating
server is refused as a whole rather than half-registered); no resources or prompts
methods; no multi-round input requests. Skill subagents never receive MCP tools --
they run concurrently and would share one server's stdin.

## The tool table has a single writer again

Saving the config dialog during a running turn re-registered every built-in tool
while the agent thread was iterating the same map: an unsynchronised read and
write over a bare `unordered_map`. The rebuild -- and the agent setters that rode
along with it -- now wait for the next turn boundary, the same window MCP tool
registration uses.

## Tests

`ctest`: 309 cases. The MCP suite spawns a real child process
(`tests/fakes/fake_mcp_server.cpp`, 12 modes) instead of mocking the protocol, and
covers registration, visibility without a prompt entry, resident reuse, stdout
noise, business errors versus protocol errors, timeouts, version rejection,
cancellation that leaves the server alive, duplicate config entries and grandchild
cleanup.

Verified end to end against `uvx mcp-server-time`: three tools, one process across
repeated calls, and a correct timezone conversion.
