# Recording raw footage

This folder holds the original screen recordings. **Nothing here is shipped or
referenced from a page** - the GIFs in `assets/` are the deliverables.

## How to record

- Terminal window fixed at **120 x 36**, font size large enough to read at
  720 px wide.
- Windows Terminal, dark theme (`Obsidian`), no transparency.
- 10-15 fps is enough when converting; record at 30 fps and let ffmpeg drop it.
- Keep each clip short: 10-20 s. Cut dead time aggressively.
- Do not show: API keys, absolute home paths, private project names, personal
  chat sessions. Use a scratch directory such as `D:\demo`.

## Files to deliver

| File | Content | Max length |
|------|---------|-----------|
| `hero.mp4` | ask a question -> tool call -> streamed answer | 20 s |
| `first-run.mp4` | launch, paste key, first message | 15 s |
| `skill-monica.mp4` | `/monica D:\demo`, several workers busy in the sidebar | 20 s |
| `skill-bigbang.mp4` | rounds of debate -> vote -> execution doc | 20 s |
| `theme.mp4` | switch theme, open editor, change a colour | 10 s |
| `image-input.mp4` | `@D:\demo\shot.png` -> answer about the image | 10 s |

## Conversion (run by the maintainer)

```bash
ffmpeg -i raw/hero.mp4 -vf "fps=12,scale=720:-1:flags=lanczos,split[a][b];[a]palettegen[p];[b][p]paletteuse" assets/hero.gif
```

Requirements: width 720 px, 10-15 fps, **<= 5 MB** per file. If it is over,
lower fps first, then trim the clip - never commit the video instead.
