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

Ten, matching the TUI's ten: approval, config, help, quit, welcome, MCP, open
session, model, theme, theme editor. One at a time; while one is open the screen
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
|- body       Row [ chat (Grow) | dock (Fixed) ]     dock toggles with F9
|- composer   the prompt, sharing the chat column's left edge, one row per line of text
|- footer     status line, queued line, service bar
\- slash      the "/" menu, floating above the composer
```

Every height comes from this frame's own measurements: how many rows the action
buttons wrap into, whether the dock is open, whether there is a queued line, how
many visual rows the prompt holds. That is why a resize lands on the frame it
happens, and why there is no `static float bottomHeight` left anywhere in the
shell.

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
  tier that fits, `wrappedRowCount` counting the rows a multi-line prompt needs (a
  newline, a wrap, a trailing newline), and the header's table: one entry per
  documented key, the toggle and role-chip labels following their state, and
  `actions::pack` wrapping instead of dropping. None of those modules includes
  ImGui, so they test without a GPU or a window.
- `tests/unit/test_worker_wait.cpp` pins the exit rule: a finished worker is joined
  (cleanup is then safe), an unfinished one is detached and the call returns at once
  (`src/core/worker_wait.h`). There is no "wait N seconds" judgement anywhere.
- `tests/unit/test_status_line.cpp` pins `Stopping...`: a cancel request outranks
  every running state, but an idle agent never shows it (one derivation, both
  front-ends).
- `proJV_studio --dump-view-model screen.json` writes the live window's snapshot;
  `proJV_tui run --jsonl` writes the same session's event stream. Diffing the two
  is how a behaviour difference is found without a human watching.

## What is deliberately different

- **Docking is enabled but has no dock host yet.** `ImGuiConfigFlags_DockingEnable`
  is set and viewports are off (D1); the layout is the TUI's fixed one until the
  blueprint editor (08.7) needs floating panels.
- **The window has a mouse and a selection model.** Text in the chat can be dragged
  over and copied with Ctrl+C (`studio/studio_text.cpp`); the "/" list is a floating
  window anchored above the prompt, so it never pushes the prompt off the screen; modal
  dialogs carry Close/OK buttons. These are additions on top of the key map, never
  replacements for it.
- **Ten dialogs, one shell.** Title bar with a close affordance, a scrolling body and
  a right-aligned action footer, sized from its content; the primary action carries
  the accent. Ten hand-sized dialogs used to clip their own text at 150% DPI.
- **Readability beats fidelity for dim text.** `studio_theme::readable()` lifts
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
| `studio/studio_modals.cpp` | the ten modals |
| `studio/studio_markdown.cpp` | `markdown_text` lines -> ImGui draw calls |
| `studio/studio_theme.cpp` | `ThemeColors` -> ImU32 (the studio's only colour conversion), fonts |
| `studio/design/actions.{h,cpp}` | the header's button table: one row per documented key, plus the wrap packing |
| `studio/design/tokens.{h,cpp}` | the size ladder (type / space / radius) and `Metrics` |
| `studio/design/layout.{h,cpp}` | the flexbox-subset solver, pure arithmetic |
| `studio/design/fit.{h,cpp}` | measuring, ellipsis, tier fitting, row counting, measurer injected |
| `studio/ui/ui.{h,cpp}` | the palette and the widget kit (button/check/radio/segmented/field/row/scroll) |
| `src/core/view_model.{h,cpp}` | the contract above |
| `src/core/speaker_slots.{h,cpp}` | the role palette both front-ends use |
