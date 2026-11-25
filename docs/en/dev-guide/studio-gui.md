# The studio (GUI front-end)

> After this page you can build the window, point at the file that draws any
> element you want to change, and prove a change kept the GUI identical to the TUI.

proJV ships two front-ends over one engine:

```
SessionRuntime (src/core/)  -- submit / run / cancel, events out. No view.
        |
        +-- proJV_tui     FTXUI     src/tui/
        +-- proJV_studio  Dear ImGui studio/
        |
   both project through view_model::Source -> Screen -> one JSON snapshot
```

The studio is not a second implementation of the product. It is a second window
onto the same state, and the shared projection is what keeps it that way.

## Build and run

Both binaries are always built; there is no switch for the GUI.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
# Windows
build/Release/proJV_studio.exe
# Linux (SDL2 is a build dependency: sudo apt-get install -y libsdl2-dev)
build/proJV_studio
```

`projv-studio` (the installer's shim) starts the window; `projv` still starts the
TUI. `--dump-view-model [file]` writes one frame's snapshot and exits -- that is
the debugging handle for "the window shows something the terminal does not".

## The view model (design 08.5, D4)

`src/core/view_model.h` is the contract:

| Piece | Meaning |
|---|---|
| `view_model::Source` | plain data a front-end fills: bubbles, agent status, panel rows, modal, input line |
| `view_model::build` | the derivations that must match the TUI byte for byte |
| `view_model::toJson` | the whole screen as ONE JSON object, keys sorted, UTF-8 kept |

The derivations that live there (and nowhere else):

- bubble -> chat row, with `chat_view.cpp`'s rules: the `── You ──` labels, the
  18-line reasoning window, the 5-line tool-result cap, the tool-call/tool-result
  merge, the vote colour, the compacted-context warning colour;
- the status line (`status_line::render`) and the status bar segments
  (`status_bar::renderSegments`);
- the `/` menu window (10 visible rows, scrolled to the selection);
- the keycap bar.

A front-end owns layout and input; it must not own wording. If a label is wrong,
it is wrong in both windows, which is the point.

## Layout parity

| TUI | studio |
|---|---|
| title `proJV (TUI)` + `[Agent: phase]` | `proJV (studio)` + the same phase line, same theme field |
| -- (no left column) | left column: Sessions (one CARD per pane, with its own CPU/MEM/GPU bars) and Files, in one stack (`studio/studio_leftbar.cpp`) |
| activity strip, 39-cell units, wrapping | same units and width; wraps at the window width |
| chat: bordered blocks, markdown, reasoning card, diff block | same rows, drawn with ImGui; markdown via the same `markdown_text` parser |
| right dock 36 cells: TODO + background tasks | same split, same width |
| status line + spinner + `[skill]` | same, spinner is the same four quadrant glyphs |
| `/` menu above the input | same rows, same 10-row window |
| input line | one WRAPPING multi-line field: Enter sends, Shift+Enter (or Ctrl+Enter) starts a new line, and the box grows one row at a time up to seven |
| `Queued: N message(s)` | same line, same colour |
| status bar segments + role pill | same segments, same colours, on the Body tier |
| keycap hints F1..Esc | NO keycap bar: EVERY key is a button in the top-right corner (hover shows its shortcut), so the two can never disagree (`studio/design/actions.h`). Two places listing the same keys read as one unfinished feature |

## Keys

Identical to `docs/*/user-guide/08-reference.md`: F1 help, F2 config, F3 new chat,
F4 open session, F5 MCP, F6 theme, F7 theme editor, F8 reveal reasoning, F9 dock,
F10 model, Tab next role, Up/Down/PgUp/PgDn scroll, Enter send, Esc = cancel /
clear / ask to quit, Ctrl+C never quits.

Every one of those eleven keys is also a button in the header, in the order the
actions are reached for rather than the order of the key numbers: Role (Tab), New
(F3), Sessions (F4), Model (F10), Reasoning (F8), Dock (F9), Theme (F6), Colors
(F7), MCP (F5), Config (F2), Help (F1). The table is `studio/design/actions.h`;
the header wraps it into extra rows when the window is narrow and never drops an
entry.

Enter/Shift+Enter is the one place the studio adds a key the TUI cannot have: a
terminal prompt is one line, a window prompt wraps. Shift+Enter and Ctrl+Enter
insert a newline, Enter validates (`ImGuiInputTextFlags_CtrlEnterForNewLine`).

The app's own key map runs **before** any widget is submitted (`App::handleKey`),
so a focused text field cannot swallow Tab or the F-keys. Arrows scroll the chat
while the prompt holds one row; once the prompt is two rows tall or more, Up/Down
belong to the caret instead (a field that scrolls the transcript *and* moves the
caret with one keypress is two things moving at once).

## Modals

Eleven: the TUI's ten plus the launcher -- approval, config, help, quit, welcome,
MCP, open session, model, theme, theme editor -- plus launch, which the TUI has no
counterpart for. One at a time; while one is open the screen
behind it takes no input. Esc closes everything except the approval dialog -- a
destructive command needs a real answer. Without an API key, Esc on the welcome
page exits (there is nothing to type into yet).

## Fonts, IME, hi-DPI (design D7)

- Monospace first, CJK merged behind it: Consolas + Microsoft YaHei on Windows,
  DejaVu Sans Mono + Noto Sans Mono CJK SC on Linux. A proportional face would
  misalign every box-drawing table the TUI relies on.
- `io.ConfigDpiScaleFonts` plus `style.FontScaleMain` from the monitor DPI; with
  ImGui 1.92+ the atlas re-rasterizes per size, so 125 % / 150 % stay sharp.
- The IME candidate window follows the caret through
  `ImGui::GetPlatformIO().Platform_SetImeDataFn` (see `setImeData` in
  `studio/main_studio.cpp`). imgui_impl_win32 1.93 does not install one, and
  without it the candidate window sits in a corner.
- A paste is flattened the way the TUI flattens it -- tabs become spaces and the
  buffer is capped -- except for the line breaks, which a multi-line field keeps: a
  pasted block can no longer send itself because Enter is the send key, not a
  character in the paste.

## The design system

The window no longer places things with numbers typed at each call site. Three
layers, and each one has exactly one job:

| Layer | File | Owns |
|---|---|---|
| tokens | `studio/design/tokens.{h,cpp}` | the only size ladder: type 28/22/18/16/13.5/12.5, space 0/4/8/12/16/24/32, radius 4/8/12/16/pill, resolved for (monitor DPI x user zoom) into `Metrics` |
| layout | `studio/design/layout.{h,cpp}` | a flexbox subset: direction, gap, padding, Fixed/Fit/Grow/Percent, min/max, cross-axis Align, main-axis Justify. Pure arithmetic, no ImGui |
| widgets | `studio/ui/ui.{h,cpp}` | one palette plus one control set: four button weights, checkbox, radio, segmented, field, list row, card, badge, keycap, scroll area |

Three rules follow from that:

1. Nothing else in `studio/` multiplies a size by a scale factor. A caller asks
   `tokens::spacePx(m, Space::Md)`, never `12.0f * dpi`.
2. A GUI colour is resolved only by `ui::palette()`, which derives it from
   `ThemeColors`. No RGB outside `studio/ui/`.
3. Text has to fit: a single-line string is measured before it is drawn
   (`fit::ellipsize`, `fit::bestTier`), and a title drops a tier when space runs out.

### Frame geometry

`drawFrame` computes every band before it draws anything:

```
window
|- header     title, phase pill, the eleven action buttons (they wrap)
|- body       Row [ left (Fixed 26%) | chat (Grow) | dock (Fixed) ]   dock toggles with F9
|  \- left    Col [ Sessions (content height) | handle | Files (Grow) ]  the chevron folds it
|- composer   the prompt, sharing the chat column's left edge, one row per line of text
|- footer     status line, queued line, service bar (+ the receipt at its right end)
\- slash      the "/" menu, floating above the composer
```

Every height comes from this frame's own measurements: how many rows the action
buttons wrap into, whether the dock is open, whether there is a queued line, how
many visual rows the prompt holds, and how many cards the Sessions panel is showing.
The Sessions panel is a minimal wrapper (its header plus one card per pane, capped at
45% of the column) so the tree gets every pixel that is left. That is why a resize
lands on the frame it happens, and why there is no `static float bottomHeight` left
anywhere in the shell.

### The left column: sessions and files

The column is the switchboard (the shape of the reference window this was designed
from): what this window is running, and what is on disk.

| Panel | Shows | Wired to |
|---|---|---|
| Sessions | one CARD per pane this window owns: title + state on the first line, then CPU / MEM / GPU as three thin progress bars inside the same card. The active card is outlined in the accent, hover fills it. Click activates, right click opens the one menu, middle click closes, `+ New` starts a pane in the folder the tree is showing | `App::activatePane` / `closePane` / `startPaneHere` |
| Files | the tree of the active pane's workspace. Double click a folder re-roots, a file sends its path; every row carries two hit boxes at its right edge, `>` (the path) and `@` (`@path`, the context form proJV already understands); `+` in its toolbar starts a session in that folder, and the context menu repeats both actions plus copy and "start a session here" | `ftree::Tree` + `path_text::payload` |

Four rules the panels keep:

1. **Rows the agent may not touch are marked.** The tree asks
   `ToolPaths::resolve`; a row outside the allowed roots is drawn dim with the
   reason in its tooltip, so a refusal is visible before the click instead of after
   the tool said `__WORKSPACE_VIOLATION__`.
2. **A number that cannot be measured is `-`, never `0`.** "GPU 0%" and "GPU
   unknown" are different claims (see `src/platform/isystem_stats.h`), and a card
   that is still starting shows `-` on all three bars instead of a measured-looking
   zero.
3. **A click has a receipt.** The last action's one-line result sits at the right end
   of the service bar for six seconds, so nothing happens silently.
4. **A row is reserved before it is drawn.** The cards and the tree rows are drawn at
   absolute rects; without a `Dummy()` reserving the row first, ImGui reports
   "SetCursorScreenPos() to extend window boundaries" and paints the whole panel in
   pure red. And a context menu is opened by a row but DRAWN ONCE after the loop --
   the same popup name queried once per row is an ImGui id conflict (that was "right
   click with two panes breaks the GUI").

### One window, several panes

A pane is one `SessionRuntime` plus its own chat, prompt line and derived Screen
(`struct Pane` in `studio/studio_app.h`). The chrome is shared and reads the active
pane. Five panes, the reference window's own limit. Consequences worth stating:

- **The workspace is per pane, not per process.** `SessionRuntime::setWorkspace`
  stores it before `initialize()`, and the seven `getWorkspaceDir()` call sites in
  `session_runtime.cpp` read that member. The default is still the launch directory,
  so the TUI and `projv run` are unchanged. Nothing calls `chdir`: one pane moving
  the process would move every other pane's tools and its snapshot store with it.
  `/undo` follows the same rule: the snapshot store is keyed by the workspace, so
  two panes on one folder share one baseline, one blob directory and one index, and
  a pane on another folder can neither see nor restore that folder's files. Turns
  stay per session: every turn carries the session database name, and a turn does
  not claim a file whose content another session's finished turn produced.
- **A busy pane refuses to close.** `closePane` answers "Esc stops it first" instead
  of destroying a `SessionRuntime` whose worker thread is still writing.
- **Starting a pane costs one frame, then ~40 ms.** Measured with per-stage timers on
  the click path: the card and its `starting...` state appear on the click frame, and
  `SessionRuntime::initialize()` (config, tools, agent, prompts, the fixed context
  blocks, MCP spawn) takes about 40 ms -- the whole click is under 100 ms. The engine
  work therefore stays on the UI thread; a worker thread for it was tried and crashes
  inside `initialize()`, and there is nothing worth moving anyway.
  What used to feel like three to five seconds was a *crash* on the same path (see the
  moved-from `unique_ptr` in `newPane`), not this call.
- **A pane created in the same folder as another gets a suffix** ("proJV @ Release
  (2)"), so two cards never read identically.
- There is no launcher dialog: a pane starts from the tree (`+` in its toolbar, a
  folder's context menu, or `+ New` in the Sessions panel, which uses the tree's
  root). The path field in the toolbar is the way to reach any other folder.

### The header is a table, not a hand-written list

The buttons used to be six literals at the draw call, and the responsive pass
DROPPED the ones that did not fit: "the buttons do not correspond to what the TUI
can do" was literally true, and it got worse the narrower the window was. Now
`studio/design/actions.h` holds one row per documented key, `actions::pack` wraps
them into rows instead of hiding them, and the frame grows by exactly the rows it
needs. `tests/unit/test_studio_design.cpp` asserts that the set of shortcuts in
that table equals the reference manual's key map, so a key added to the product
without a button fails the test run instead of the user's expectation.

## Parity testing

- `tests/unit/test_view_model.cpp` pins the derivations and cross-checks them
  against the TUI's own renderer: every label the view model produces must appear
  verbatim in `chat_view::renderBubbles`'s output for the same bubbles.
- `tests/unit/test_studio_design.cpp` pins the geometry: a monotonic type ladder,
  clamped scaling, row/column/percent/grow/min-max rectangles, CJK counted as two
  columns, ellipsis that never splits a code point, `bestTier` choosing the largest
  tier that fits, `wrappedRowCount` counting the rows a multi-line prompt needs, the
  header's table (one entry per documented key, stateful labels, wrap-don't-drop),
  the file tree (`nameKey` order, folders first, `b2` before `b10`, a real directory
  read, expansion depth), the path payload's four quoting cases, the gauge
  arithmetic (bytes, `-` for unknown, a ceiling that scales) and the turn clock. None
  of those modules includes ImGui, so they test without a GPU or a window.
- `cpuPercentBetween` is pinned the same way: one second of CPU over one second of
  wall clock on four cores is 25%, a tree that shrank clamps to 0, a zero-length
  window has no rate.
- `tests/unit/test_worker_wait.cpp` pins the exit rule: a finished worker is joined
  (cleanup is then safe), an unfinished one is detached and the call returns at once
  (`src/core/worker_wait.h`). There is no "wait N seconds" judgement anywhere.
- `tests/unit/test_status_line.cpp` pins `Stopping...`: a cancel request outranks
  every running state, but an idle agent never shows it (one derivation, both
  front-ends).
- `proJV_studio --dump-view-model screen.json` writes the live window's snapshot;
  `proJV_tui run --jsonl` writes the same session's event stream. Diffing the two
  is how a behaviour difference is found without a human watching.

## What the per-session numbers can and cannot say

ISystemStats is fed one group of pids per session, so a card's CPU/MEM/GPU belong to that session's own child processes: its background tasks (bg_task::Snapshot::pid), its MCP servers (mcp::Manager::serverPids) and, when exactly one session is running a tool, the single child the runner holds right now (IProcessRunner::CurrentPid). Two limits are stated rather than hidden:

- **The tool child is attributed only while one session is busy.** IProcessRunner is one child at a time by design (src/platform/iprocess_runner.h); with two sessions queued for it, the running child belongs to the one whose turn executes it.
- **GPU is - on Linux.** The kernel exposes per-card utilisation (amdgpu's gpu_busy_percent), not per-process; claiming a session's share out of a card number would be a guess dressed as a measurement. On Windows the PDH instance names carry pid_<n>, so the figure is real there.

### A tool block folds to one line

The chat draws a tool call as one line -- a triangle, the first call, the outcome
and the size of what is folded away -- and opens on click. The rule that decides
which bubble belongs to which row already lived in the derivation, so the fold is
derived there too (`view_model::ChatRow`: `key`, `collapsible`, `collapsed`,
`summary`, `status`, `diffSummary`, `diffText`, `extraFull`, `lines`).

- A row is identified by `key` (`"<msgId>:<k>"`, built in `studio_app.cpp` where
  the bubbles are appended), never by its index: a streaming append and the
  oldest-200 trim both shift an index.
- `view_model::Source::toolsCollapsed` is the switch, `expandedKeys` the rows the
  user opened -- the same shape as `reasoningExpanded`, so `build()` stays a pure
  function and the snapshot stays a golden test.
- A writing tool's diff is absorbed into its own row. It used to stop the merge
  loop, so with two calls in one turn the second result fell out as a "Result"
  card and each diff became a card of its own. The TUI still draws the diff as a
  separate block; the same change there is a separate decision.
- `extraFull` carries the result body (`Bubble::body`); `extra` keeps the TUI's
  five-line cap of the compact display string.
- The click is hit-tested AFTER `EndChild` (the child's rect is known there), and
  the frame it consumes never reaches `TextSurface::handleInput`, or one press
  would both fold the row and start selecting text in it.

`proJV_studio --screenshot out.png [--frames N] [--demo-chat] [--expand KEY]
[--scroll-up N]` renders through the real draw path, reads the back buffer and
writes a PNG. No image library is involved: `studio/png_writer.cpp` writes stored
deflate blocks (see `tests/unit/test_png_writer.cpp`). `--demo-chat` fills a fixed
transcript, so an acceptance image never depends on a live model.

### The footer belongs to the transcript

The prompt and the service bar are the session's own furniture, so they live in
the transcript's column: the side panels keep their full height, and the footer
starts at the prompt's left edge instead of the window's. The status line moved
out of the stack above the prompt into the column beside it (state on top, the
turn's own clock or the skill pill under it), which is what bought the prompt its
second row: `composerRowCount` never returns less than two.

Following the end of the transcript is a LATCH the reader owns (`App::followEnabled`):
a wheel turn away from the end or a scrollbar drag turns it off, wheeling back to
the end turns it on, and nothing else touches it. While it is on, the chat tracks
the bottom on every frame (`ui::scrollToBottom`). Content arriving never arms
anything -- three earlier designs did, and each died the same way: they had to ask
"is the reader far away?" on a frame where ImGui reports the content height one
frame stale (imgui.cpp, "Update contents size from last frame"), so a view that was
merely catching up to a new bubble looked far away and gave up for good. A frame
trace showed the transcript 1.3 screens short after a minute of streaming.
`--demo-stream N` appends a demo bubble every N frames, which is how that was
reproduced without a model.

The service bar is per pane: its workspace comes from `Pane::workspace` (it used
to print the window's launch folder), the receipt lives on the pane, and a session
card asks the pane for its live phase instead of reading the pane's `Screen` --
only the active pane's Screen is rebuilt each frame, so every other card kept a
phase from the last time it was looked at. The studio also makes the TUI's own "a
turn ended" sound (`tui::beepIdle`, on the same falling edge `main_tui.cpp` uses).

The spinner is a drawn arc with a frame-count phase (so a screenshot run stays
reproducible), not the TUI's `|/-\` glyphs, which read as a stray character
beside a label. Both context menus push their own window padding before
`BeginPopup`: a popup reads that value when its window starts, so a padding push
inside the block does nothing and the labels run into the border.

## What is deliberately different

- **A window owns several sessions.** The TUI is one session per process; the studio
  can hold panes side by side, each with its own workspace. That is a studio feature,
  not a shared derivation: the panes' state lives in `studio/studio_app.h`, and the
  `view_model` contract gained no field for it.
- **Resource sampling has its own thread and no child process.** `ISystemStats`
  reads counters (Toolhelp32 + `GetProcessTimes` + PDH on Windows, `/proc` + sysfs on
  Linux). It never goes through `IProcessRunner` -- that runner is one child at a
  time by design, and making a sample queue behind a 30-minute shell command would
  be useless. A GPU counter set that does not exist is reported as unknown, with the
  reason, instead of a fabricated 0%.
- **Per-pane CPU/MEM is NOT attributed when several panes share one process.** Their
  shell children hang off the same parent, so the System panel reports the whole
  tree; a per-pane figure would be a guess dressed as a measurement.
- **No layout persistence yet.** The column's width, the split ratio and the tree
  root live in memory, the same way the window has no `imgui.ini`.
- **Docking is enabled but has no dock host yet.** `ImGuiConfigFlags_DockingEnable`
  is set and viewports are off (D1); the layout is the TUI's fixed one until the
  blueprint editor (08.7) needs floating panels.
- **The window has a mouse and a selection model.** Text in the chat can be dragged
  over and copied with Ctrl+C (`studio/studio_text.cpp`); the "/" list is a floating
  window anchored above the prompt, so it never pushes the prompt off the screen; modal
  dialogs carry Close/OK buttons. These are additions on top of the key map, never
  replacements for it.
- **Eleven dialogs, one shell.** Title bar with a close affordance, a scrolling body and
  a right-aligned action footer, sized from its content; the primary action carries
  the accent. Eleven hand-sized dialogs used to clip their own text at 150% DPI.- **Readability beats fidelity for dim text.** `studio_theme::readable()` lifts
  secondary text to at least a 4.5:1 contrast ratio against the window background.
  The TUI's "disabled" grey is 2.7:1 on the Light theme, which is fine on a terminal
  and unreadable in a window.
- **Body text is 16 px at 100%.** It was 14.5 in the first pass, which on a 4K panel
  at 100% meant leaning in to read a chat line; the whole ladder was re-cut at a ~1.2
  ratio, readable first and hierarchical second.
- **No deadline on quitting, and no cleanup when it cannot be trusted.**
  `SessionRuntime::shutdown()` asks one question -- has the worker finished? Finished:
  join it, then close the database, reap subprocesses, shut MCP down. Not finished:
  detach it and clean up **nothing**, because the worker may still be writing the
  session database and closing it under a live writer is a use-after-free
  (`src/core/worker_wait.h`). The OS reclaims the address space; SQLite replays its
  write-ahead log next start. An earlier version waited 5 s and then cleaned up
  anyway, which was the worst of both shapes: a random-feeling pause and a race.
- **Esc is visible immediately.** `AgentStatus::cancelling` is set by
  `Agent::cancel()` and rendered by `status_line::render` as `Stopping...`, so both
  front-ends show that the request landed on the next frame instead of looking dead
  for several seconds.
- **Every blocking wait in the turn path is cancellable.** Waiting for a parallel
  tool slot used to be a `cv_.wait` that never looked at the cancel flag, so a worker
  parked there could not be woken; it now waits on "a slot is free **or** cancelled"
  (`src/core/sub_agent.cpp`). Same shape as the approval wait, which already did this.
- **The prompt is one control, not a card with a field inside, and it is multi-line.**
  The input frame is the box: theme surface fill, one hairline border, token rounding
  and padding, and `ImGuiCol_NavCursor` pushed transparent. A card behind it plus
  ImGui's focus highlight is what produced two outlines that could never align. The
  field wraps (`ImGuiInputTextFlags_WordWrap`), holds line breaks, and the frame
  sizes it from the text before drawing it -- up to seven rows, then it scrolls.
- **The footer sits on the Body tier.** The status line and the service bar were Small
  (13.5 px), twice reported as too small to read; a terminal has one text size, so the
  GUI's footer now matches the size the chat is read at, and only the badges stay
  small.
- **A multi-line paste keeps its line breaks.** The single-line field had to flatten
  them away (a pasted block could send itself); with Enter as the send key and
  Shift+Enter as the newline, a pasted block can no longer send anything. Tabs still
  become spaces, and the paste cap is the TUI's.
- **Layout is cached per row**, so a long conversation is not re-parsed every frame
  (that quadratic cost was what made the window stop responding during a skill run).
- **No `imgui.ini`.** The layout is fixed, so there is no state file to drift.

## Testing the window without a human

- `build/studio.ps1` launches/stops the app through `Start-Process`. Plain `start`
  leaves the GUI holding the shell's stdout pipe, so the command never returns until
  the window is closed -- that is what "the kill does not work" looked like.
- `build/shot.ps1` declares per-monitor DPI awareness before capturing. A DPI-unaware
  PowerShell photographs a downscaled 2560x1440 desktop on a 4K/150% panel, and the
  bottom bars shrink into near-black pixels.
- `build/drive.ps1` switches the target window to an en-US layout before `SendKeys`.
  With a Chinese IME active, the preedit buffer swallows injected characters.
- `PROJV_STUDIO_DEBUG_LAYOUT=1` writes `layout.log` (section cursor positions, clip
  rect, frame heartbeat) and `wnd.log` (raw Win32 key messages). Those three files are
  how every layout and input defect in this phase was found.

## Files

| File | What |
|---|---|
| `studio/main_studio.cpp` | Windows: WGL context, IME hook, main loop |
| `studio/main_studio_sdl.cpp` | Linux: the same loop over SDL2 |
| `studio/studio_app.{h,cpp}` | state, actions, key map, `Source` filling |
| `studio/studio_ui.cpp` | the main screen: chat, dock, status, the wrapping header, input |
| `studio/studio_leftbar.cpp` | the left column: Sessions, Files, System, the drag handles |
| `studio/studio_modals.cpp` | the ten modals (no launcher: a pane starts from the tree) |
| `studio/studio_markdown.cpp` | `markdown_text` lines -> ImGui draw calls |
| `studio/studio_theme.cpp` | `ThemeColors` -> ImU32 (the studio's only colour conversion), fonts |
| `studio/design/actions.{h,cpp}` | the header's button table: one row per documented key, plus the wrap packing |
| `studio/design/file_tree.{h,cpp}` | the tree model: Explorer order, lazy levels, visible rows |
| `studio/design/path_text.{h,cpp}` | the path -> prompt text rule (`>`, `@`, quoting) |
| `studio/design/gauges.{h,cpp}` | bytes / percent / fraction / ceiling arithmetic for the System panel |
| `studio/design/work_clock.{h,cpp}` | turn elapsed time and throughput, timestamps injected |
| `src/platform/isystem_stats.h` | the sampling interface (no child processes, unknown is not zero) |
| `src/platform/system_stats_{win,linux}.cpp` | the two implementations (Toolhelp32 + PDH / `/proc` + sysfs) |
| `studio/design/tokens.{h,cpp}` | the size ladder (type / space / radius) and `Metrics` |
| `studio/design/layout.{h,cpp}` | the flexbox-subset solver, pure arithmetic |
| `studio/design/fit.{h,cpp}` | measuring, ellipsis, tier fitting, row counting, measurer injected |
| `studio/ui/ui.{h,cpp}` | the palette and the widget kit (button/check/radio/segmented/field/row/scroll) |
| `src/core/view_model.{h,cpp}` | the contract above |
| `src/core/speaker_slots.{h,cpp}` | the role palette both front-ends use |
