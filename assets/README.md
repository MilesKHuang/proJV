# assets

Everything the README and the docs embed.

| File | Used by | Notes |
|------|---------|-------|
| `logo.jpg` | repo settings, social preview | the source logo |
| `demo.gif` | `README.md`, `README_zh.md` hero | current recording |
| `hero.gif` | README hero (planned) | replaces `demo.gif` when recorded |
| `social-preview.png` | GitHub Settings -> Social preview | 1280 x 640, to be produced |
| `wechat_donate.jpg` | README support section | unchanged |
| `raw/` | not shipped | source screen recordings, see `raw/README.md` |

## Rules

- Embeds use relative paths (`assets/xxx.gif`), never absolute local paths.
- A GIF is only committed when it is under 5 MB; otherwise lower the frame rate
  or trim the clip, do not commit the video.
- Source recordings stay in `raw/` and are not referenced from any page.

## Pending

- [ ] `hero.gif` - ask -> tool call -> streamed answer, <= 20 s
- [ ] `first-run.gif` - launch, paste key, first message, <= 15 s
- [ ] `skill-monica.gif` - `/monica <dir>`, several workers busy in the sidebar
- [ ] `skill-bigbang.gif` - debate rounds -> vote -> execution doc
- [ ] `theme.gif` - switch theme, open the editor, edit a colour
- [ ] `image-input.gif` - `@path` and the answer
- [ ] `social-preview.png` - 1280 x 640

Each of these is referenced from the docs by its final name, so dropping the
file in and deleting the placeholder note is the whole swap.
