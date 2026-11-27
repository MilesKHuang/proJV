# v0.8.2 - what is highlighted is what is copied

Two fixes. One is the defect that survived 0.8.1: a drag over Chinese still put
the band a few characters away from the pointer, and what `Ctrl+C` produced did
not always match the band. The other is a rule the two front-ends now share --
while a turn runs, the keys that would change the session do nothing, and they
say why.

No new features. The engine, the event protocol, the skill format and the
config/session files are unchanged.

## One ruler for the pointer, the band and the clipboard

0.8.1 fixed the heading face and gave every visual line the character width it
was wrapped with. That was still one number per line. A monospace Latin face and
a merged CJK face do not share a cell size -- `中` is its own box, not two Latin
cells -- so "column x width('M')" drifts by a fraction of a cell per glyph, and
over a line of Chinese the caret ends up several characters from the pointer.

Every visual line now records where each of its characters starts, measured once
at layout time by the font that will draw it (a new ImGui-free file,
`studio/design/text_runs.{h,cpp}`). The hit-test, the highlight and the clipboard
read those offsets and nothing else: three readers of one ruler instead of three
guesses. The clipboard also reads the lines actually drawn this frame rather than
walking an append-only layout cache, so a block that is on screen with no band on
it can no longer turn up in the paste.

Three consequences worth knowing:

- A soft-wrapped paragraph is copied as one paragraph, byte for byte. The space a
  break drops sits at the end of the line above, so nothing glues together.
- A table is selected and copied as one block: the whole box is banded and the
  cells come back tab-separated. It deliberately gets no per-character ruler --
  ImGui's table widget places those glyphs, and this string does not.
- A folded tool row is not drawn, so a selection that spans it skips its body.
  The band shows the reader exactly what the paste will contain.

## Session keys wait for the turn

`F3` (new chat) and the `Enter` of the `F4`/`F10` pickers cancel the turn and
close the database; `Tab` rewrites the system prompt; saving `F2` rewrites config
and the tool table under the worker. Every one of those raced the turn, and the
first two could lose the conversation. They now do nothing while a turn runs and
put one line on screen: `busy -- finish the current turn first`.

That sentence lives in one place (`view_model::busyRefusal()`). It used to be a
literal in the TUI's MCP panel while the window said `refused: a turn is running`
-- one condition described in two ways by two front-ends. The MCP panel keeps
listing while busy and refuses `Enter`/`R`; opening the `F4`/`F10` pickers is
read-only and stays allowed. `SessionRuntime` keeps a backstop on `newChat`,
`switchToDialog` and `setModel`, so a caller the front-ends do not know about
cannot race the worker either.

## Tests worth naming

- `tests/unit/test_studio_copy.cpp` pins the arithmetic that turns measured
  offsets into characters and characters into clipboard text, including a
  backwards drag, a soft wrap and a selection that claims to reach a line that
  was never drawn. No ImGui include.
- `tests/copy_selftest.cpp` (ctest name `studio_copy_probe`) drives the real
  `TextSurface` through `beginDraw` + `drawBlock` + `handleInput` with real ImGui
  mouse and key events over a real monospace + CJK face, and reads the clipboard
  from ImGui's own callback. It is the one test that can show a CJK advance is not
  two Latin cells, because it needs the font to say so.

Both run in `ctest`. `proJV_tests`: 564 cases, 8347 assertions, all green.

## Upgrade

Nothing to migrate: no config key, session schema or skill format changed. Drop
the new binary (or package) in place; it keeps the `projv_files` next to it.
