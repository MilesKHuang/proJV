# v0.7.10 - the agent without a window

The orchestration left the TUI. Everything that runs a turn -- the turn loop, the
slash commands, the skill phase, the MCP tool-table sync, the session bookkeeping
-- now lives in one class that a terminal, a script or a future GUI can drive. The
terminal front-end kept the drawing and nothing else: 1407 lines became 269.

The other half of this release is about a lie the skill engine told itself. To
finish, a driver agent had to answer with the single word `DONE`; then the same
agent was asked to write the report, and sometimes answered `DONE` again. That is
gone: completion is a parameter now, and the report rides on the same call.

## `projv run`: the same agent, no terminal

```
projv run -p "say hi" [--jsonl] [--no-ts] [--max-turns N]
                       [--session PATH] [--approve ask|never|always]
                       [--dump-request] [--schema]
```

`--jsonl` emits the event stream (`src/core/events.h`): one JSON object per line,
`t` is the event name, empty fields are omitted. `--no-ts` drops timestamps, so
the same prompt twice produces byte-identical output -- that is the regression
test. `--schema` prints the event table. Exit codes: `0` ok, `1` error, `2`
cancelled, `3` an approval was needed but `--approve never` refused it.

With no dialog available, a supervisor thread answers every approval request the
way `--approve` says, so a headless run can never hang waiting for a click.

## SessionRuntime owns the loop

`src/core/session_runtime.{h,cpp}` holds the one `AppConfig`, the agent, the tool
registry, the path guard, the MCP pool, storage, the background-task registry,
the skill runner and the pending queues. Three ways in -- `submit()`, `run()` /
`startAsync()`, `cancel()` -- and one way out, `onEvent`.

`TuiApp` holds bubbles, scroll, theme and dialogs, and forwards. The two never
race: `run()` and `startAsync()` latch on one flag, and the callbacks fire on the
turn thread, where a front-end queues a redraw instead of touching widgets.

## Skills finish by setting `finish`

The verb tool gained a boolean:

```json
"stop_when": { "flag": "finish" }
```

One call now carries both the report and "the job is done": if the payload is a
real report it becomes the deliverable and the converge step is skipped (one
model call saved); if it is a bare acknowledgement, the converge step still runs
as a fallback. The legacy `{slot, equals}` form still parses, so a skill you did
not migrate keeps working.

Two supporting fixes came with it:

- A prose report the model already wrote is no longer thrown away when the tool
  payload is a stop-word. Only the stop-word list counts -- a short verdict like
  "ship it" is still a real answer.
- The converge retry now tells the agent what was wrong instead of re-sending the
  identical prompt, which could never succeed.

A finished run also says so: the last line in the chat is now
`[skill] finished after N round(s): report delivered (N chars)`, and the round
that carried the finish flag no longer leaves a misleading "dispatch" marker
behind it.

monica, musk and avenger are migrated; bigbang and steve already converged on
`all_agree` and are untouched.

## Clean-up

- Five symbols verified dead by grep across `src/` and `tests/` were deleted:
  `SessionRuntime::launchAgentThread`, `skillThread_` / `joinSkillThread`,
  `TuiApp::runtime()`, `mcp::Manager::workDir()` / `loadedPath()`,
  `snapshot::unavailableReason()`.
- The module map, the run reference and both authoring guides now describe the
  code as it is: one runtime instead of a TUI-owned loop, `finish` instead of a
  magic word, five shipped skills instead of two.
- `AGENTS.md`: adding a vendored dependency is now a table row instead of a wall,
  and the colour rule names `ThemeColors` instead of a directory.

## Upgrading

The shipped skill configs changed. A binary reads skills from `projv_files/skills/`
next to the executable, and `ensureDefaultSkills()` only seeds a missing folder --
it never refreshes one. Copy the new `skills/` over the old tree, or move the old
folder aside and let the binary re-seed it.

`config.toml`, session databases and themes are untouched.

## Tests

461 cases, including five new ones for the deliverable contract: the stop-word
list, the payload check, and one that fails if a shipped skill's config, verb
schema and role prompt disagree about `finish`.
