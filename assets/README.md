# assets

Everything the README and the docs embed.

| File | Used by | Notes |
|------|---------|-------|
| `logo.jpg` | repo settings, social preview | the source logo |
| `demo.gif` | `README.md`, `README_zh.md` hero | **this is the hero** - no separate `hero.gif` needed |
| `social-preview.png` | GitHub Settings -> Social preview | 1280 x 640, to be produced from `logo.jpg` |
| `wechat_donate.jpg` | README support section | unchanged |
| `raw/` | not shipped | source screen recordings, see `raw/README.md` |

## Rules

- Embeds use relative paths (`assets/xxx.gif`), never absolute local paths.
- A GIF is only committed when it is under 5 MB; otherwise lower the frame rate
  or trim the clip, do not commit the video.
- Source recordings stay in `raw/` and are not referenced from any page.

## Pending

- [ ] `first-run.gif` - launch, paste key, first message, <= 15 s
- [ ] `skill-monica.gif` - `/monica <dir>`, several workers busy in the sidebar
- [ ] `skill-bigbang.gif` - debate rounds -> vote -> execution doc
- [ ] `theme.gif` - switch theme, open the editor, edit a colour
- [ ] `image-input.gif` - `@path` and the answer
- [ ] `social-preview.png` - 1280 x 640

`demo.gif` already covers the hero slot, so the README has no placeholder left.
Each remaining clip is referenced from the docs by its final name; dropping the
file in is the whole swap.
