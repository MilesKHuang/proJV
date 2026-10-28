# proJV v0.6.3 Release Notes

> Compare: `v0.6.0` -> `v0.6.3`
> Upgrade from any earlier build by replacing `proJV_tui` and `projv_files/`.
> No config migration needed; one new optional key (`reasoning_effort`).

---

## Highlights

### Reasoning effort is now yours to set

`config.toml` gained `reasoning_effort`, the OpenAI-compatible field DeepSeek
documents for Thinking Mode:

```toml
reasoning_effort = "max"   # none / low / high / max
```

- The values are exactly the official enum. `minimal`, `medium`, `xhigh` and
  `ultra` are accepted and normalised the way the API maps them.
- `none` is the documented way to switch thinking **off**. proJV translates it
  into `{"thinking":{"type":"disabled"}}` and does not send an effort value,
  because the docs forbid sending both controls at once.
- Default stays `max`; an unknown value falls back to the official default
  `high` instead of being sent to the API.
- Note that `temperature` has no effect in thinking mode (official behaviour,
  not a proJV limitation).

### Every conversation starts with your AGENTS.md

Put an `AGENTS.md` in the workspace root and it is injected into the session as
a fixed `[AGENTS.MD] ... [END AGENTS.MD]` block:

- build/test commands and house rules reach the model without you repeating them;
- only the root file is read (no recursion, no `CLAUDE.md` fallback), capped at
  16 KiB with an explicit truncation marker;
- takes effect in the next conversation (`F3`) or after `/clear`;
- skills do not receive it, on purpose: ten concurrent workers would each pay
  for another copy.

This release also fixes the two bugs that made the old behaviour inconsistent:

- `F3` (new chat) used to drop the directory tree, so the model lost the
  workspace layout;
- `/clear` used to clear the system prompt as well and never restore it, so the
  following requests went out with no system message at all.

`/clear` now restores the system prompt, the directory tree and `AGENTS.md`, and
keeps the current session file and the visible history.

### The model can look at an image itself

On a Flash model the agent has a new `view_image` tool. It does not return
pixels: the API only accepts images inside `user` messages, so the tool registers
an image round, the turn ends, and proJV appends a synthetic user message
carrying the attachments. The next turn can see them.

- The message is rendered as `── You (auto) ──` and stored with
  `synthetic = 1`, so replaying a session never looks like you typed it.
- `view_image` must be called alone; a turn's other tools still run and keep
  their results, but the round stops right after them.
- `view_image` and `run_skill` cannot be queued in the same round.
- Same guards as `@path`: workspace confinement, 8 images, 30 MiB per file,
  44 MiB total, JPEG/PNG/GIF/WebP only.

### Token estimates match the tokenizer again

The context estimate counted UTF-8 **bytes**: a Chinese character was charged
1.5 tokens instead of about 0.6, so Chinese sessions compacted roughly 2.5x too
early and the advertised 1M window was unreachable in practice.

- Now counted per character, with the documented 0.3 token/ASCII char and
  0.6 token/CJK char.
- One implementation (`src/core/token_estimate.h`) shared by the context
  pressure calculation and the stored `messages.token_count` column, so the two
  can never drift again.

---

## Everything else

- Model list fallback no longer offers retired model names; only
  `deepseek-v4-flash` and `deepseek-v4-pro` remain.
- `modelSupportsTools` no longer keys off a retired name; every current model
  supports tools.
- Dynamic system messages (todo, overview, user request) are inserted right
  after the fixed system block instead of position `[1]`, matching the
  compaction rule: the system prompt, directory tree and `AGENTS.md` keep their
  indexes, so DeepSeek's prefix cache keeps hitting.
- `AGENTS.md` added to the repository root: build/test commands and the rules an
  agent must not break, for any agent working on proJV itself.
- Docs: `reasoning_effort` in both config tables, an `AGENTS.md` section in
  daily use, a `view_image` section in images, injection points in the module
  map, README highlights 1M context and the effort knob.
- Tests: 184 -> 207 cases (three new files `test_token_estimate`,
  `test_agents_md`, `test_view_image`, plus reasoning-effort and seed-upgrade
  assertions), 943 assertions.

## Verification

- `cmake --build build --config Release` clean on MSVC 19.29 (Windows).
- `ctest --test-dir build -C Release --output-on-failure`: 207/207 cases,
  943 assertions.
- Headless end-to-end harness (`tests/e2e/`, opt-in via `-DPROJV_BUILD_E2E=ON`)
  drives the real `TuiApp` against a fake API: 22/22 checks, including the
  captured request/response shape of the `view_image` round.
- Manual checks (real key needed) are listed in `04_VERIFY.md`.
