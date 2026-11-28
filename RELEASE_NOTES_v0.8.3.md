# v0.8.3 - a finished background task wakes the model, idle or not

One feature and one defect it exposed. A detached task used to be reported only
if the turn that started it was still running, so the common case -- start a long
build, let the turn end, wait -- ended in silence: no line on screen, no sound,
and a model that never learned its build had finished. Now the task reports
itself, a running turn or not.

No new tool, no new file format, no config key. The event stream gains the `task`
events that were already in its schema.

## One report, whoever is awake

The registry's monitor thread calls a finish hook with the id of every task that
ends -- exited, timed out, or stopped with `/end_task`. The runtime files those
ids in one slot, and whichever side is awake drains it: a turn that is still
running takes the batch at its round boundary and keeps going in that same turn;
an idle session starts a turn of its own. The batch is ONE message with one entry
per task, so three tasks ending together cost one model call, not three.

Three details that make it usable rather than merely present:

- **A kill counts as a finish.** `/end_task 3` used to leave the model waiting on
  an id that would never speak again. It now hears that the task is dead.
- **Your line always goes first.** If you typed something while the task was
  running, your turn starts and the report rides along in it instead of taking a
  turn ahead of you.
- **The wake is a real turn.** It gets a turn number, an `/undo` snapshot and the
  end-of-turn beep, and if it runs into an approval prompt it waits for you like
  any other turn.

A windowed front-end runs its own frame loop and reaches the slot by itself; the
terminal draws only when something invalidates the frame, so the finish hook also
asks for a repaint. That one line is the difference between "reports when you
touch the keyboard" and "reports by itself".

## A guide that described code that never ran

The developer's module map said the turn loop reported finished ids through
`turn_requests` (`Kind::Process`). No such path existed -- the enum value was
never claimed by anything, and the reporting lived inline in the loop. The map now
describes what runs, the dead enum value is gone, and the user reference says what
actually happens when a background task ends.

## Tests worth naming

- `tests/unit/test_task_registry.cpp` pins the three things this release rests on:
  several ids merge into one block with no blank line between entries, an id with
  no snapshot still gets its line (silence would leave the model waiting
  forever), and the finish hook fires exactly once for a child that exits -- and
  once for a task stopped by `end()`.

`proJV_tests`: 567 cases, 8361 assertions, all green. `ctest` runs it plus the
studio copy probe.

## Upgrade

Nothing to migrate: no config key, session schema, skill format or tool
signature changed. Drop the new binary (or package) in place; it keeps the
`projv_files` next to it.
