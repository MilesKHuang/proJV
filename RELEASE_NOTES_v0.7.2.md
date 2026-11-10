# v0.7.2 - the prompt finally knows what it can do

## A role prompt built for reading, not for showing off

The default seed (`projv_files/prompts/assistant.md`) now opens with the answer
shape and makes it the highest-priority rule: conclusion first, in plain language
a new graduate can follow; then what it means for the user; then the concrete
details (paths, signatures, line numbers); then one next step. Precision is still
demanded -- it just no longer comes first.

- The hand-written tool inventory is gone. It had already drifted: it listed
  `diagram_tool` whether or not a pytool was installed.
- The old seed claimed tool results over ~500 characters get truncated. The real
  cap is 32000; the wrong number pushed the model into `exec_shell` detours.
- The `tools available:` line left the seed as well. A custom role may still add
  it to narrow its own tool list, and omitting it still means "no restriction".
  The seed explains that rule without spelling out the trigger string, because the
  parser searches for the literal and would read the surrounding prose as a list.

## A capability map, rebuilt every turn

Every request now carries a `[CAPABILITIES]` block generated from what is actually
registered at that moment: built-in tools grouped by area with their hard limits,
installed skills with the description from each `config.json`, and MCP servers with
the purpose text from `mcp.json` plus the tool names they contribute.

- Parameter names and types are not repeated -- they are in the tool definitions of
  the same request. The map says what a tool is for and what it cannot do.
- Output is sorted, so two consecutive turns are byte-identical and only a real
  change in the tool table can interrupt the KV prefix cache.
- It is never stored in the session database, which is exactly why it cannot go
  stale when a skill appears or an MCP server becomes ready mid-session.
- Skills degrade by count (descriptions up to three, one clipped line up to six,
  names only above that), and a name in a role allowlist that is not installed is
  rendered `(not installed)` instead of silently disappearing.

## One place builds the system messages

`src/core/context_assembler.{h,cpp}` owns the layout. The environment block
(workspace, `projv_files`, toolchains, the access matrix) and the project block (a
600-character directory tree plus `AGENTS.md`, capped at 8 KiB with a truncation
notice that says where to read the rest) are written once per session; the
capability map and the per-turn todo/overview blocks are inserted after them.

Those three fixed messages are the anchor for both compaction and the per-turn
block, so they are written even when a field is empty. Startup, `/clear` and `F3`
all go through the same function, which is what keeps the prefix identical.

## `projv_files` is writable, history is not

Extensions live next to the binary and the workspace is the launch directory, but
the file tools only accepted paths under the workspace -- so "add a skill" was
impossible. A `ToolPaths` object now carries the matrix: workspace and
`projv_files` (prompts, skills, theme, pytool, `mcp.json`) are readable and
writable, `projv_files/sessions` and `projv_files/logs` are read-only, and anything
else is refused with a message that lists every allowed root.

## `mcp.json` takes effect on the next message

Editing the file used to require a restart. It is re-read before a turn starts and
compared by fingerprint (`command` + `args` + `env` + `protocol` + `trusted` +
`timeout_ms`), so only entries that really changed are touched.

- A description-only edit never restarts a process.
- A half-saved file keeps every running server: the parse error is recorded and
  shown in `F4`, and the resident list is left alone.
- A deleted server's tools stay callable until the turn boundary, where the single
  registry writer unregisters them, and the handle is dropped afterwards.
- A `Dead` server is not retried on every message; `F4`'s `R` stays the retry entry.
- `description` is now effectively required -- it is the only place the model can
  learn what a server does, so a missing one is a warning.

## Smaller things

- `skill_maker` is gone. The authoring guide ships as
  `projv_files/skills/_authoring/skill-authoring.md`: readable by the agent, not
  registered as a runnable skill.
- `mcp_manager.h` no longer includes `agent.h`. `syncTools()` reports what it
  changed and the caller applies it, which also keeps `session.h` and SQLite out of
  every MCP compile unit.
- A tidy-up pass removed eight symbols that were defined and never used, and the
  house `.clang-format` was applied to the files that had drifted.

## Tests

`ctest`: 327 cases, 6577 assertions. New suites are `test_capabilities` (the map
names exactly the tools the request carries, byte stability across registration
order, skill degradation, bounded size), `test_tool_paths` (the permission matrix,
the read-only roots, an error that lists every root) and a rewritten
`test_prompt_seed` that greps the seed for each hard rule, so losing one is a red
test rather than a slow degradation.

## Known limits

- Deleting an MCP server and adding it back under the same name in one session
  leaves it dormant until a restart: the reload reuses the removed handle without
  clearing its flag. A fix is planned; restarting proJV is the workaround.
- The capability map is capped at 4000 characters. A project with many skills and
  several MCP servers will see its tail replaced by a pointer to the source files.
