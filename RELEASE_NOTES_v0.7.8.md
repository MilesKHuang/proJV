# v0.7.8 - work that finishes

Three milestones in one release. **M2** made long output, long commands and long
tasks survivable; **M2.5** added the thing every user assumes is there (type `/`, see
what exists); and the approval model was rebuilt after a day of dogfooding made it
obvious that "ask about everything destructive" was asking about the wrong thing.

## Long output keeps its conclusion

Both truncation layers kept the **head**, so `2 errors generated` and `FAILED:` were
the first thing thrown away. They now keep the tail, and say so:

- `exec_shell` returns the last 400 lines by default (`tail_lines`, 10-2000), with a
  header naming how many lines were omitted. `full_output: true` keeps everything up
  to 200 KB -- the context budget, not a display preference -- and tells the model how
  to narrow a command with a pipe.
- The platform runners no longer cut at 32 KB from the front; they keep 4 MB with a
  middle notice (a memory guard, not a display policy).
- Cuts back off to the last complete UTF-8 character, so multi-byte output never turns
  into mojibake.
- pytool had its own 32 KB head cut, missed by the first pass, now shaped the same way.

## Background tasks

`exec_shell(background: true)` detaches a command and returns `bg#N` immediately. The
child writes straight to a per-task file -- a pipe nobody drains deadlocks the child
once the OS buffer fills, which is exactly what a build or a download does. A 60 minute
cap, a dock line with the latest output, `task_output(N)` to read it, `/end_task N|all`
to kill the process tree. Windows adds a Job Object so a proJV crash takes the children
with it. ESC still only detaches from a wait; killing is deliberate.

The three follow-up turn sources (skill, image, background report) are a queue now
instead of a single slot that silently dropped the second request.

## /wish

`/wish <what you want>` tells the model to pick the open decisions itself, record each
one (`wish_decide`), and keep working until its own TODO list is empty. It stops on:
TODO done, two idle passes, `wish_block` (the model says what is missing), 50 passes, or
`/wish cancel`. The recorded decisions are printed when it stops. It never ends a turn
with a question -- that is the whole point.

## Type `/` and see what exists

The command list appears above the input: builtins plus installed skills, filtered by
prefix first and substring second, arrow keys to move, Enter to fill the command
(not send), Esc to close. Skills are read from their `config.json`, so a new skill shows
up without a restart. The list scrolls with the selection -- a fixed "first 8" would have
hidden half of it behind an unreachable "… more".

## Approval: three stops, and a memory

`auto_approve` was a boolean trying to express three positions, and "Always" only
remembered an exact command until `/clear`. Replaced by:

```toml
approval_mode = "balanced"   # strict | balanced | loose
approval_grants = ["git push"]  # written when you click ALWAYS
```

- **strict** -- every delete, overwrite, redirect and `git commit`/`push` asks, and
  there is no ALWAYS button at all.
- **balanced** (default) -- destructive commands ask; build output such as
  `build > log.txt` and read-only one-liners such as `py -3 -c "import sqlite3; ..."`
  run without a dialog.
- **loose** -- only formatting/partitioning is refused.
- **Deny is outside the dial.** `format`, `diskpart`, `mkfs.*`, `fdisk`, `parted`,
  `wipefs`, `sgdisk`, `dd of=/dev/*`: no dialog, no override, in every mode.
- ALWAYS writes the exact command into `config.toml`, so it survives a restart and
  `/clear`. `/approval` lists them, `/approval clear` forgets them.

## Capability map and budget

- The map is no longer truncated as a whole. It used to be cut at 4000 characters from
  the tail, which is exactly where Skills and MCP sat -- so a well-configured project
  lost the part it had configured. Each section is bounded on its own now (MCP tool
  names capped at 10 per server, pytool descriptions dropped above 12 tools).
- The fake "30 tool calls per round" counter is gone. It never stopped a turn; it
  injected `[Batch complete. Continuing...]` and reset. Runaway loops are caught by the
  consecutive-failure check, which now fires at two failures instead of thirty.
- `max_tokens` is clamped to `[1024, 65536]` on load, and the retry after
  `finish_reason=length` never lowers it (it used to halve anything above 65536).

## Skills: four failures found by running one for real

- **A stale cancel flag killed runs silently.** ESC, F3, F5 and `/clear` latch it;
  only a new turn cleared it, and a skill never starts a turn. Ten rounds became zero
  output with no message. A skill now starts un-cancelled, and says so if it cannot.
- **A silent round left no trace.** A supervisor that dispatched without prose produced
  no chat line at all: monica ran six rounds and the UI showed one. Every round now
  leaves one line (`Round N/M`, plus `-- no narration from the model` when it was
  silent).
- **`DONE` was accepted as the deliverable.** The final step is asked for a report; the
  model often answers `DONE` again, and that became the "result". It is retried once
  with a blunt instruction, then reported as *no deliverable produced* instead of
  pretending.
- **The shared context was the whole transcript.** Every sub-agent carried all of it --
  276k tokens per agent in a long session. It is a bounded digest now: the three
  per-turn blocks plus the newest 12 KB, compressed with the compactor's own prompt
  when it is still large, and the blocks are kept after the summary so the goal cannot
  be summarised away. Read-only: a skill can never rewrite the conversation.

## F2 edits everything

Every key `config.toml` holds except `[model_prices]`: API key, base URL, model,
`max_tokens`, temperature, `reasoning_effort`, `context_window`, the three toolchain
paths, the two snapshot keys, theme, `osc_status`, the approval mode and the remembered
command list (with a clear button). Values are clamped exactly where the loader clamps
them. Esc or F2 closes; Save/Cancel sit in a pinned footer, so a taller dialog can never
push its own exit off the screen (it did, once).

## Upgrade notes

- `auto_approve` is retired; it is still read as `true` -> balanced, `false` -> strict,
  and erased from the file on the next save.
- Grants now persist. A command you allowed forever in 0.7.5 lived until `/clear`; from
  now on it is a line in `config.toml` -- `/approval` shows what is in there.
- `workspace_path` stays retired: the workspace is the directory you launch proJV from.
- No data migration. `projv_files/logs`, `snapshots` and the session databases are
  unchanged.
