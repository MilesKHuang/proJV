# assets

Everything the README and the docs embed, plus the GitHub social preview.

| File | Used by | Notes |
|------|---------|-------|
| `logo.jpg` | source image | 779x780, black interior with a #4971B7 frame |
| `social-preview.png` | `README.md`, `README_zh.md` hero; GitHub Settings -> Social preview | 1280x640 card: logo on black, title, tagline, accent rule. Displayed at 780px wide in the READMEs |
| `demo.gif` | README, under the badges | the demo. No separate `hero.gif` is planned |
| `wechat_donate.jpg` | README support section | shown at 150px wide |
| `raw/` | not shipped | recording instructions, kept for later |

## Rules

- Embeds use relative paths (`assets/xxx.gif`), never absolute local paths.
- A GIF is only committed when it is under 5 MB; otherwise lower the frame rate
  or trim the clip, do not commit the video.
- Source recordings stay in `raw/` and are not referenced from any page.

## Regenerating the social preview

`social-preview.png` is produced from `logo.jpg` (script kept out of the repo):
black 1280x640 canvas so the logo's black interior blends seamlessly, logo at
400x400 on the left, `proJV` in Segoe UI Bold, tagline and a #4971B7 rule on the
right. Text must stay pure ASCII: a PowerShell script saved without a BOM is read
as ANSI, so a `·` becomes two Chinese glyphs in the image.

## Not doing (decided for v0.6.0)

The five extra action GIFs (`first-run`, `skill-monica`, `skill-bigbang`,
`theme`, `image-input`) are **not** part of this release. The README needs no
placeholder for them, so nothing is broken by leaving them out. `raw/README.md`
keeps the recording instructions in case they are wanted later.
