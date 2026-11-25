# v0.8.0 - the transcript that folds

A chat log is mostly noise: ten tool calls, their results, and three lines of
answering. The studio's transcript now folds that noise instead of stacking it.

The rest of the release is smaller and mostly invisible: a tool call records how
long it took, a skill run records which run it belonged to (both persisted, both
display-only), the studio can photograph itself for a review, and the vision path
no longer dies with HTTP 413 on a conversation that has screenshots in it.

The TUI is unchanged by this release -- its renderer still draws a separate diff
block and does not fold. That is a deliberate scope choice, not an oversight.

## A tool block is one line

```
> write_file: src/core/view_model.cpp   ok  1.4s  +7 -2
> exec_shell: cmake --build build       ok  12.4s
> format_file                           failed  780ms
```

Each row is one assistant message's calls: the call itself, whether it worked,
how long it took, and the size of what is folded away (a diff's `+N -M`). Click
the row to open it: the full result text and the diff, in the same card. The
result text shown when open is the real thing -- before this release the chat had
only ever received a one-line summary of it.

## A skill run folds per role

```
> Analyst  -  run #1  -  2 msgs
> Critic   -  run #1  -  2 msgs
> Writer   -  run #1  -  1 msg
```

Grouping comes from an id the run stamps on every message it writes, so a
replayed session groups exactly as the live one did. "run #n" is counted in
transcript order; nothing extra is stored for it.

## Following the end

The transcript follows the end while the reader is at the end, and stops the
moment the wheel moves away; wheeling back resumes it. Content arriving never
moves the view by itself. (Three earlier attempts armed the follow when content
arrived and then asked "is the reader far away?" -- a question that cannot be
answered correctly on a frame where the content height is one frame old, so a
view that was merely catching up gave up for good.)

## Window

- The prompt and the service bar span the transcript **and** the dock, which now
  ends level with the transcript. The side panels keep their full height.
- The prompt is two rows tall by default: one line to write, one to see.
- The status line (state, plus the turn's own clock and throughput) sits beside
  the prompt instead of above it.
- The wordmark is a small muted label reading `proJV studio <version>`, not the
  loudest element in the window.
- The spinner is a drawn arc, not `|/-\`.
- `tok/s` is measured over a two-second window of the text actually arriving.
  It used to divide prompt+completion by the turn's length, which is why the
  first second of a 30k-token context read as "30000 tok/s".

## Vision: HTTP 413

A request carries the whole history, and an attachment is re-encoded from its
path on the way out, so a pasted screenshot rides along on every later turn.
Enough of them and the gateway rejects the body -- with no way back, because the
history cannot shrink. `Session::stripOldImages` trims the REQUEST copy only (the
database and the bubbles keep their images), and the per-message cap drops from
44 to 32 MiB to leave room for the transcript.

## Screenshots for review

```
proJV_studio --screenshot out.png [--frames N] [--demo-chat]
                                  [--expand KEY] [--scroll-up N] [--demo-stream N]
```

Renders through the real draw path, reads the back buffer, writes a PNG -- no
image library involved. `--demo-chat` fills a fixed transcript so an image never
depends on a live model; `--demo-stream` keeps appending bubbles, which is how the
follow behaviour was debugged.

## Upgrade

The session database gains two columns (`duration_ms`, `skill_run_id`) through the
same `ALTER TABLE` migration the `diff` column uses, so an existing session opens
as before and its tool rows simply show no clock. Nothing new is sent to the API:
both fields are display-only and `to_json` writes its keys explicitly.

## Fixes worth naming

- `Calling PopFont() too many times`: a `###` heading popped a font it never
  pushed, and a guarded push with an unguarded pop went negative when a face
  failed to load.
- Every prose paragraph was drawn as a fenced code block (a style test accepted
  "Normal" as "same style"), so ordinary text sat on a full-width code band.
