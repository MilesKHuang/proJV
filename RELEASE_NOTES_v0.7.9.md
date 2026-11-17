# v0.7.9 - keys that mean what they say

One release about the interface lying to itself: Ctrl+C killed the process while
the hint bar advertised help, the "About" box was documented as Help, F4 and F5
had swapped meanings in the code comments, `/help` forgot two commands that
existed, and a pasted stack trace sent the first line the moment it arrived.
Nothing in the model layer changed.

## Ctrl+C is not an exit any more

Getting this right took four layers, and three of them are not proJV's own code:

- The console handler answers `CTRL_C_EVENT` and `CTRL_BREAK_EVENT` with TRUE
  (handled, do nothing) and only acts on `CTRL_CLOSE_EVENT`, so closing the
  window still exits and still checkpoints the database.
- FTXUI installs its own CRT handler with `std::signal(SIGINT, ...)` inside
  `Loop()`, and Windows calls console handlers last-in-first-out: that one runs
  *before* ours and turns a real Ctrl+C into `Exit()`. `ForceHandleCtrlC(false)`
  does not cover it. proJV replaces it with `SIG_IGN` on the first frame after
  `Install()` -- that is the line that actually keeps the process alive.
- `ENABLE_PROCESSED_INPUT` is deliberately left **on**. Clearing it does turn ^C
  into an ordinary byte, but the console's "Ctrl+C copies the selection" and
  "Ctrl+V pastes" live on that same path: with it off, a terminal stops copying
  what you selected. Ctrl+C is neutralised without touching the mode instead.
- The startup sequence spawns child processes (python detection, skill setup, MCP
  handshakes) and one of them resets the console input mode, so `initTerminal()`
  runs again just before the loop.
- A root-level `CatchEvent` swallows `Event::CtrlC` as a last resort, and the
  input box handles `^V` itself by reading the clipboard, which keeps paste
  working in hosts that forward the key instead of pasting.

Closing paths now: `Esc` -> confirm box -> Quit (`shutdown()`, WAL checkpointed and
closed), or the window's X (`emergencyShutdown()`, the OS gives it 5 seconds).
Ctrl+Break deliberately does nothing: several laptop keyboards have no such key,
and it has no time limit, so a stuck handler would remove the only way out.

A forced exit may leave a `-wal` file next to the session database. That is
expected: SQLite replays it on the next open, and every committed message is
already on disk (one IMMEDIATE transaction per message, no write queue).

## Esc has three meanings, and none of them is "exit"

Busy: cancel the turn. Idle with text: clear the line (cursor included). Idle and
empty: a "Quit proJV?" box, with Cancel focused and `Esc` acting as Cancel.
`Ctrl+C` and `Ctrl+V` still copy and paste through the terminal, which is where
they belong.

A first run with no API key used to have no keyboard exit at all once Ctrl+C was
gone; the welcome overlay now quits on `Esc`.

## Agents moved out of the dock

The dock carried a two-line block per agent and a heading, which cost five rows
before the first agent appeared and reflowed nothing when the terminal got
narrower. Skill agents now live in a strip under the title bar:

- one cell per agent (`[name] <spinner> <last narrated line>`), fixed width,
  wrapped by FTXUI, so a 120 column terminal shows three and an 80 column one
  shows two, with no code to handle the resize;
- an idle app keeps a single `(no active agents)` line, so the chat area above
  does not jump when a skill starts;
- order is the agent id's order from the skill config, stable while the skill
  runs -- nothing re-sorts itself frame by frame.

The dock keeps the TODO list (`F9` toggles it) and gains one line per running
background task, same cell shape, `bg#N` in front. A running build and a running
skill must not look like the same thing.

## One command table

`/help`, the `/` menu and the new `F1` panel now read `src/core/help_text.cpp`.
The old `/help` table was a second copy and had already lost `/approval` and
`/workspace`; both appear now. F1 replaces the About box: it lists every command
and every key, scrolls with Up/Down, and closes with `Esc` or `F1`. It has no
buttons on purpose -- a panel taller than the terminal that hides its own exit
button is exactly how the F2 dialog once became impossible to close.

The reference manual (`docs/*/user-guide/08-reference.md`) is still written by
hand, checked by eye; nothing generates it.

## Keys that were lying

| Key | Was | Is |
|---|---|---|
| `F1` | About | Help (commands + keys) |
| `F4` | MCP servers | open an old session |
| `F5` | open session | MCP servers |
| `F8` | labelled "think" (it only expands the reasoning) | "reveal" |
| `F9` | labelled "todo" | "dock" (TODO + background) |

Docs, both READMEs and the code comments were corrected with them, including two
comments that had said F11 for the model picker since it moved to F10.

## Smaller things

- The spinner is the same four quadrant glyphs everywhere (FTXUI charset 8) and
  advances every 120 ms instead of every 16 ms animation callback: 480 ms per
  revolution instead of 210 ms of flicker. Every call site goes through
  `src/tui/ui_spinner.h`.
- The last four hard-coded colours (status spinner, queued notice, approval
  command echo, vote red/green) come from the theme. A theme switch now moves all
  of them; the vote red/green match the diff's removed/added colours.
- `F2` -> Effort is four radio buttons (`none` / `low` / `high` / `max`). Old
  values keep working: `minimal` maps to `low`, `medium`/`xhigh` to `high`,
  `ultra` to `max`; anything unreadable falls back to `high`, the official
  default, not to `none`.
- A pasted block no longer sends its first line: within a burst (gaps under 5 ms,
  about an eighth of the fastest OS key repeat) each Return becomes a space, and
  a paste past 200 000 characters stops converting instead of growing.

## Known residuals

- The activity strip grows from one row to four when monica starts; with the chat
  pinned to the bottom the top of the viewport loses a few lines, and a manually
  scrolled view slides by a row or two. Accepted for this release.
- Under ConPTY (Windows Terminal, VS Code) there is no QuickEdit: copying is the
  host's own shortcut (`Ctrl+Shift+C`, right click).
- Pressing Ctrl+C with nothing selected does nothing at all. That is the point,
  but it needs saying.
- A hard kill can lose the turn that was generating; committed messages are safe.
- No `/copy`: the keyboard-only fallback was dropped after the host selection path
  was verified on both platforms.
