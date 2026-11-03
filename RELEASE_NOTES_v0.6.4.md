# proJV v0.6.4 Release Notes

> Compare: `v0.6.3` -> `v0.6.4`
> Upgrade by replacing `proJV_tui` and `projv_files/`. No config migration.
> `projv_files/fonts/` is dead weight now: nothing reads it, delete it if you
> still have one.

---

## Highlights

### The bundled fonts are gone -- proJV draws with the OS font

The package used to carry a font payload under `projv_files/fonts/`: `msyh.ttc`
plus the whole LXGW WenKai / WenKai Mono webfont subset set -- 584 files, 28 MB,
84% of the archive. Nothing in the binary ever opened a single one of them.

proJV is a TUI. FTXUI only emits escape sequences; the terminal owns the glyphs,
and layout width comes from the UTF-8 lead byte (`markdown_view.cpp`,
`displayWidth`), never from font metrics. So text is already rendered in
whatever font the OS and the terminal are configured with, and shipping a copy
could never change that.

- `ISystemUtil::GetSystemFontPath()` is removed with both implementations,
  including the Linux Noto / WQY / Droid `.ttc` probe list. It had no caller in
  `src/` or `tests/`, and it looked under `{exeDir}/assets/`, not
  `projv_files/fonts/`, so it never found the payload either.
- `projv_files/fonts` is no longer a `KeepIfMissing` tree in the installer, and
  the docs (en + zh) drop it from the directory tree and the upgrade table.
- Release ZIP 14.22 MB -> 1.68 MB; staged install tree 22.25 MB -> 3.47 MB.

### One command installs it, and `projv` lands on your PATH

- A user-level install (no administrator rights): the new
  `docs/en|zh/user-guide/01-install.md` walks the ZIP route and the cmake route
  through the same end state.
- The installed binary writes the `projv` forwarder itself:
  `proJV_tui --post-install` (`--install` / `--uninstall` also exist). The
  download-a-ZIP user and the `cmake --install` user end up identical, and
  installing the shim can never fail the install itself.
- CPack staging never creates a command on the build machine: the shim step is
  skipped when the prefix matches `_CPack_Packages`.
- The upgrade table states exactly what a second install does: exe / README /
  LICENSE replaced; `projv_files/{theme,pytool,skills}` only added when
  missing, so your edits survive; `config.toml`, `sessions`, `logs`, `prompts`
  never touched.

### Your terminal can show what the agent is doing (OSC 7501)

- New `src/core/terminal_status.{h,cpp}`: the agent reports its phase to the
  host terminal with the OSC 7501 program-status sequence, so a tab title or a
  status indicator can show busy / waiting / done without a second window.
- Driven by a single `Agent::setPhaseCallback` hook, wired at the TUI call
  sites; `osc_status` in `config.toml` switches it off (`true` by default).
- Terminals that do not implement the sequence ignore it, so there is no
  fallback path to keep in sync.

### You can write a skill without touching C++

- `docs/en|zh/user-guide/09-skills-authoring.md` (both languages): the complete
  `config.json` schema, one `.md` prompt per agent, `tools/<verb>.json`, the
  turn / `emit` protocol, template variables, `stop_when`, `all_agree`,
  `max_tool_iters`, the driver rule, two worked examples and a pre-flight
  checklist. It is written so an agent can produce a working skill from it.
- The `skill_maker` skill is compiled into the binary and self-seeds on first
  launch: it writes a new skill's files, then a verifier role reads them back.

### A three-skill SOP pipeline: avenger -> steve -> musk

- Three **data-only** skills, zero engine change: `avenger` researches (Stark
  dispatches parallel research lines, audits them, writes one summary doc),
  `steve` designs (expert lines plus an engine-enforced `all_agree` vote),
  `musk` executes (dev x test pairs against a one-line DoD, judged on quoted
  evidence).
- `skills/README.md` documents the pipeline, the folder contract and the
  one-person path.

---

## Everything else

- `fix(skill,config)`: the main agent now reviews a skill's output, and the
  workspace is the launch directory instead of the exe directory.
- `skills/steve`: explicit vote discipline and an action inventory; a
  substantive dissent cannot be reasoned away (`skills/musk`).
- README and README_zh rewritten: one centered header block in both languages,
  `projv` on PATH, OSC 7501 and Ostlr surfaced. The Chinese README was mojibake
  and is now valid UTF-8.
- The Ostlr design baseline moved to its own repository; the install / shim
  design appendix is archived at `docs/archive/projv-install.md`.
- Tests: 238 cases / 1217 assertions (207 -> 238 cases since `v0.6.3`), three
  new files (`test_osc7501`, `test_install_shim`, `test_skill_sop`).

## Verification

- `cmake --build build --config Release` clean on MSVC 19.29 (Windows).
- `ctest --test-dir build -C Release --output-on-failure`: 238/238 cases,
  1217 assertions, 0 failed.
- CI runs the same suite on Windows and Ubuntu for every push to `main`, and
  again inside `release.yml` before CPack is allowed to run: a package is never
  built from a red tree.
