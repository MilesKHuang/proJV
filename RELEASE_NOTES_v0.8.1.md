# v0.8.1 - the answer stops looking like a form

Three fixes, all of them things a reader notices before anything else: the status
bar's turn clock kept disappearing, selected text could not be seen, and every
answer arrived as a filled-in four-part form.

No new features. The engine, the event protocol and the skill format are
unchanged, and so is the TUI.

## The turn's numbers stay on the bar

The status bar printed `12.4s  34 tok/s` only while a turn was running, so the
moment the answer ended both numbers vanished -- look away for a minute and there
was nothing left to read. `work_clock::end()` now freezes the turn's length in
`finalElapsedMs`, and the rate keeps the last figure its two-second window
measured. The next turn's `begin()` replaces both.

The rate also stopped collapsing to 0. The window was fed the **length of the
streaming buffer**, and that buffer is emptied at the start of every round (once
per tool call) and does not move at all while a tool runs, so the figure kept
falling back to zero in the middle of an answer. It is now fed a monotonic
emission count: a new round restarts the count from what is on screen, and a tool
phase holds the last real figure instead of blanking it.

## One face per block, and a selection you can see

Two defects with one root. A markdown heading was drawn at Title (22 px) or
Heading (18 px) on the **proportional** face, while the very same line had been
wrapped and hit-tested in the 16 px monospace face -- three different rulers on
one line, which is why a click landed a few characters away from the pointer.
Headings are now body size, bold, in the `mdH1`/`mdH2`/`mdH3` colour, which is
exactly how the TUI draws them (it never pushes a size either). Every visual line
records the character width it was wrapped with, so the click, the highlight and
the clipboard share one ruler.

The highlight moved to the foreground draw list, clipped to the chat rect. It
used to go on the chat window's own list, where each bubble -- a child window,
rendered after its parent -- painted over the very text the highlight marked.
That is why selection looked like it did nothing.

Smaller, same area: a drag that leaves the glyphs snaps to the nearest line
instead of stalling at the edge of the text; a streaming block takes its id from
the same sequence as the settled rows, so a copy across both comes out in visual
order (the fixed `-11/-12` numbered the live block before the whole transcript);
and the selection is skipped while a modal is open, since a foreground highlight
would otherwise be painted on top of the dialog.

The ordering and span arithmetic moved to `studio/design/text_select.{h,cpp}`,
which has no ImGui include and seven unit tests behind it. Nothing about the
selection needs a GPU to be checked any more.

## The answer is prose, not a form

The seed spelled the answer out as `1. Conclusion / 2. What it means for the
user / 3. Details / 4. Next step`, and a model fills a form in: every reply grew
the same four headings whether or not it had that much to say. It now asks for
the same order as prose -- conclusion first, then what it means for the user,
then the details, then the next step, and no heading for a part that has nothing
in it.

Two literals stay in the text, because they are the rules the form was carrying:
`exact file paths, function signatures` and `new graduate can follow`. A test
pins both, plus the absence of the four numbered headings.

## Upgrade

The seeded `assistant.md` is rewritten as a whole on the next start, anchored on
the sentence that opened the four-item list, and the previous text is kept next
to it as `assistant.md.bak`. A prompt you wrote yourself is left to the style
upgrade -- lose a hand-written prompt once and you never trust an upgrade again.

A running session keeps the old prompt until it restarts; prompts are read at
startup.

## Fixes worth naming

- Selecting text copied in the wrong order across the streaming block, because
  its id was negative.
- `studio_theme` carried `TextRole` / `fontPx` / `pushText` / `popText` with no
  caller left after the heading change. Removed rather than left as an invitation
  to reintroduce a second ruler.
