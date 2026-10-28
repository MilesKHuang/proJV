## What changed

<!-- One or two sentences. Link the issue: Fixes #123 -->

## Why

<!-- The problem, not the diff. Reviewers read this first. -->

## Type

- [ ] feat
- [ ] fix
- [ ] docs
- [ ] build / CI
- [ ] refactor (no behaviour change)

## Checklist

- [ ] `ctest --test-dir build -C Release --output-on-failure` passes locally
- [ ] new or changed behaviour is covered by a test (bug fix: the test fails
      before the fix)
- [ ] docs updated in **both** languages (`docs/en/` and `docs/zh/`)
- [ ] if build steps or the "do not" rules changed: `AGENTS.md` updated too
- [ ] `README.md` / `README_zh.md` updated if user-visible surface changed
- [ ] no secrets: no `config.toml`, no `projv_files/sessions/`, no keys, no logs
- [ ] if this touches the skill engine: UI callbacks still only after a join,
      one turn per agent, every tool call still gets a result
- [ ] if this touches tools: every path still goes through `validateToolPath`

## Notes for review

<!-- Anything you are unsure about, or deliberately left out. -->
