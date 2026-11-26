# Repository settings (paste-ready)

These are the texts to paste into the GitHub web UI. Everything here is prose,
no code changes.

## Description

English (<= 120 chars):

```
Native C++ AI agent core: one core, two front-ends, ~4 MB, no runtime, DeepSeek inside.
```

Chinese (<= 120 chars):

```
原生 C++ AI agent 核心：一份核心、两个前端、约 4 MB、零运行时，内置 DeepSeek。
```

## Topics

```
ai-agent
tui
cpp
cpp20
deepseek
terminal
multi-agent
ftxui
llm
cli
```

## Social preview

Upload `assets/social-preview.png` (1280 x 640) in
**Settings -> General -> Social preview**.

Content: logo on the left, `proJV` + one line on the right. Keep at least 40 px
of margin, because GitHub crops the card on some surfaces.

## Website

Leave empty, or point to the Releases page:

```
https://github.com/MilesKHuang/proJV/releases
```

## First release

1. Tag and push:
   ```bash
   git tag v0.6.0
   git push origin v0.6.0
   ```
2. `release.yml` builds, tests, packages both platforms and creates the Release.
3. Paste `RELEASE_NOTES_<tag>.md` into the Release body (the workflow generates
   notes from commits; the curated file is better), and add:
   - Windows: unsigned binary, SmartScreen -> "More info" -> "Run anyway".
   - Linux: `sudo apt-get install -y libcurl4-openssl-dev libsdl2-dev` if libcurl or SDL2 is missing.

## Branch protection (recommended)

`main`: require the CI check, require a PR, no force-push.
