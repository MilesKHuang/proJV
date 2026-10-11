# proJV v0.5.6 Release Notes

> Date: 2026-10-09
> Compare: `v0.5.2` -> `v0.5.6`

---

## Highlights

### Skill engine -- installable multi-agent workflows

proJV now ships a generic multi-agent **skill engine**. A skill is just a folder
under `projv_files/skills/` containing a `config.json`, one `.md` prompt per
agent, and `tools/` verb definitions. The engine treats every folder the same --
no built-in skills, no special-casing in code.

- Run a skill: `/skill <name> <topic>` -- e.g. `/skill bigbang add git diff`.
- List installed skills: `/skill list`.
- Install = copy a folder into `projv_files/skills/`. Uninstall = delete it.

The workflow is data, not code: `config.json` describes agents, `round_steps`
(with `parallel` batches), blackboard capture slots, and convergence.

### Built-in `skill_maker`

A `skill_maker` skill is compiled into the binary and self-seeds into
`projv_files/skills/` on first launch (mirroring how default prompts are
seeded). Run `/skill skill_maker <idea>` and the agent authors a new skill for
you (config + prompts + tools) and verifies it.

### Example skills: bigbang and monica

Two complete example skills ship in `skills/` and are deployed next to the
binary at build time (and packaged into the release):

| Skill | What it does |
|-------|--------------|
| `bigbang` | three-role engineering debate (Sheldon / Penny / Leonard) -> one execution document |
| `monica`  | supervisor + workers (formatter / translator / auditor) project tidy-up |

### Multi-agent UI

The TUI grew a dedicated agent sidebar: agent status is now a two-line panel,
speaker colors are registry-driven from the active skill's roles, and role
replies are rendered as distinguishable chat bubbles.

### One-click packaging / release automation

`clean_and_build.bat` now builds **and packages** in one step. `CMakeLists.txt`
gained `install()` rules plus a `CPack` ZIP configuration, producing
`proJV_v0.5.6_win64.zip` that contains `proJV_tui.exe` and a **complete**
`projv_files/` (fonts, pytool, theme, skills). Dependency install rules
(FTXUI / SQLiteCpp / libcurl) are suppressed so no third-party headers or
static libs leak into the package, and `__pycache__` / `*.pyc` are excluded.

> Users only need to keep `proJV_tui.exe` and `projv_files/` together; a fresh
> folder copies straight to a machine and runs. `prompts/`, `sessions/` and
> `config.toml` are created and seeded on first launch.

---

## Changes by Area

| Area | Before (v0.5.2) | After (v0.5.6) |
|------|-----------------|----------------|
| Multi-agent | one-off `/bigbang` debate | generic `/skill` engine driven by `config.json` |
| Extensibility | prompts + pytool only | + installable skills (`projv_files/skills/`) |
| Examples | none | bigbang, monica (shipped + packaged) |
| Skill authoring | manual | built-in `skill_maker` self-seeds |
| Agent UI | single agent | agent sidebar, role-colored bubbles, two-line status |
| Packaging | manual, incomplete zip | CMake `install()` + CPack, complete `projv_files/` |
| Release build | build only | `clean_and_build.bat` builds + packages |

---

## File Changelog

### Added

| File | Purpose |
|------|---------|
| `src/core/skill_runner.{h,cpp}` | generic multi-agent skill engine |
| `src/core/skill_config.h` | skill `config.json` model |
| `src/core/skills_builtin.{h,cpp}` | built-in `skill_maker`, seeded at launch |
| `src/core/sub_agent.*` | `request_agent` / swarm plumbing |
| `src/tui/` agent panel + speaker-color files | agent sidebar and role coloring |
| `skills/bigbang/**`, `skills/monica/**` | example skills |
| `RELEASE_NOTES_v0.5.6.md` | this document |

### Modified

| File | Key Changes |
|------|-------------|
| `CMakeLists.txt` | version 0.5.6; `install()` rules; CPack ZIP; suppress dep install rules |
| `clean_and_build.bat` | added `cpack` packaging step |
| `src/core/agent.cpp` | version banner 0.5.6 |
| `src/tui/main_tui.cpp` | About box version 0.5.6 |
| `README.md`, `README_zh.md` | new Skill engine section |

### Removed

| File | Reason |
|------|--------|
| `RELEASE_NOTES_v0.5.2.md` | superseded by v0.5.6 |
| `BIG_BANG_DEBATE*.md`, `SKILL_ENGINE.md`, `SUBAGENT_FIX.md` | scratch design docs, folded into code |
