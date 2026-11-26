# Recording raw footage

This folder holds the original screen recordings. **Nothing here is shipped or
referenced from a page** - the GIFs in `assets/` are the deliverables.

## Shipped today

| GIF | Source recording here | What it shows |
|-----|----------------------|---------------|
| `assets/demo.gif` | `studio-session.mp4` | the window front-end: a question, thinking, the streamed answer |
| `assets/demo-tui.gif` | `tui-session.mp4` | the terminal front-end: the same exchange |

Both were recorded with Win+Alt+R (Game Bar) on a 2560-px-wide display, then
trimmed and converted with the commands below. The README shows each at 480 px
side by side and **links to the full-size GIF**, because at 480 px the text is a
shape rather than a sentence.

## How to record

- Windows Terminal, dark theme (`Obsidian`), no transparency.
- 5-12 fps is enough when converting; record at 30 fps and let ffmpeg drop it.
- Keep each clip short: 10-20 s. Cut dead time aggressively - the first seconds of
  a clip are usually typing, and typing is not the demo. Both current clips start
  after the message is sent for exactly that reason.
- Do not show: API keys, absolute home paths, private project names, personal
  chat sessions. Use a scratch directory such as `D:\demo`.
- An IME swallows injected keystrokes; type by hand, or switch the window to en-US
  before scripting `SendKeys`.

## Conversion (run by the maintainer)

The trim window is the important part: both clips spend the first ~18 s typing,
and the demo only starts once the message is sent.

```bash
ffmpeg -ss 18.2 -i raw/studio-session.mp4 -an \
  -vf "fps=5,scale=960:-1:flags=lanczos,split[a][b];[a]palettegen=max_colors=128[p];[b][p]paletteuse=dither=bayer:bayer_scale=3" \
  -loop 0 assets/demo.gif

ffmpeg -ss 19.0 -i raw/tui-session.mp4 -an \
  -vf "fps=5,scale=960:-1:flags=lanczos,split[a][b];[a]palettegen=max_colors=128[p];[b][p]paletteuse=dither=bayer:bayer_scale=3" \
  -loop 0 assets/demo-tui.gif
```

Requirements: 960 px wide, 5-12 fps, **<= 5 MB** per file - the pair above lands at
4.45 MB and 2.30 MB. If a clip is over: lower fps first, then trim, never commit
the video instead.

## Files worth recording next

| File | Content | Max length |
|------|---------|-----------|
| `skill-monica.mp4` | `/monica D:\demo`, several workers busy in the sidebar | 20 s |
| `embed-jsonl.mp4` | `projv run --jsonl` streaming, the line that proves the engine | 15 s |
| `theme.mp4` | switch theme, open editor, change a colour | 10 s |
