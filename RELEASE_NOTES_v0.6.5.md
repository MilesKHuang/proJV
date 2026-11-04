# proJV v0.6.5 Release Notes

> Compare: `v0.6.4` -> `v0.6.5`
> Upgrade by replacing `proJV_tui`. No config migration, no theme re-import, no
> session migration: an old `.db` opens unchanged and its stored diffs replay.

---

## Highlights

### A file write now shows what it changed

`write_file`, `edit_file`, `md_file` and `format_file` snapshot the file around
their own write and record the unified diff. The TUI shows it in its own bubble
under the tool call, so a write is no longer a one-line "Written 195 bytes".

- **Wide chat column (>= 100 usable cells): two panes.** Old line numbers and
  text on the left, new line numbers and text on the right, a changed line
  paired with its replacement on the same row -- the layout an editor's diff
  view uses. Narrower terminals get the single-column unified block instead; the
  switch is automatic, there is no key to press.
- **Both gutters are real file line numbers**, so `@@ -13,3 +13,3 @@` is
  verifiable at a glance.
- **`-` / `+` markers stay inside each pane**, so a colour-blind theme still
  shows which side changed. Colours are mapped from existing theme fields
  (`todoDone` / `statusError` / `textDisabled`), so
  `projv_files/theme/*.json` needs no edit and no re-import.
- **Context is 2 lines per side** (3 in the narrow layout); a block is capped at
  40 rows, and the tail says where the full patch lives.
- A `format_file` that changes nothing records nothing: no bubble for a no-op.
- The `F9` TODO dock takes 36 cells, which can drop the chat column below the
  split threshold. That is the intended fallback, not a bug.

### The diff never reaches the model

It rides on the tool row, not inside the tool's `content`, and `to_json` does
not serialize it. So it costs no input tokens and cannot disturb the
conversation: it exists to be looked at.

- `messages` grows a `diff TEXT` column, added by the same try-`ALTER TABLE`
  block that already migrates `reasoning_content`, `image_paths` and
  `synthetic`. Databases written before this release open and read as before.
- Replay keeps it: `F5` on an old session re-renders the same diff, in whatever
  layout the current window fits.
- The capture slot is `thread_local`, not a registry member: skill phases run
  SubAgent workers on parallel threads that share one `ToolRegistry`, so a
  member would let two workers overwrite each other's change.

### `doc/` is an agent artifact directory, not documentation

The prompts write design documents to `doc/`, so the directory is now ignored
(`/doc/`, anchored at the repo root) instead of accidentally tracked. Its only
tracked content was `projv-architecture.{dot,png}`, referenced from nowhere.

Alongside it, the archived install note is no longer part of the repository:

- `docs/archive/` stays on a developer's disk but is untracked, and
  `docs/archive/projv-install.md` was removed from the index.
- All eight in-repo references to it are gone (README, README_zh, the v0.6.4
  notes, a CMake comment, two `install_shim.h` comments, `main_tui.cpp` and
  `test_install_shim.cpp`). The reasoning stays; only the path went away.
- The rule is written down where it will be read: `AGENTS.md`,
  `CONTRIBUTING.md` and `CONTRIBUTING_zh.md` now say that `docs/archive/` is
  never committed and never linked from a committed file, and that rationale
  which must be public belongs in `docs/en/` + `docs/zh/`.

---

## Everything else

- Removed `b2.txt`, a committed MSBuild log from an old build, referenced from
  nowhere.
- `tests/CMakeLists.txt` now links `SQLiteCpp` for the test target only, so
  `test_storage_diff` can build a pre-migration schema by hand. `proJV_lib`
  keeps the dependency `PRIVATE`.

## Verification

- `cmake --build build --config Release` clean on MSVC 19.29 (Windows).
- `ctest --test-dir build -C Release --output-on-failure`: 274/274 cases,
  1400 assertions, 0 failed.
- New test files: `test_diff_capture` (unified diff writer), `test_diff_parse`
  (two-column alignment), `test_storage_diff` (column round trip and migration).
  Extended: `test_file_tools`, `test_bubble_model`, `test_reasoning_content`,
  `golden/test_chat_render`.
- CI runs the same suite on Windows and Ubuntu for every push to `main`, and
  again inside `release.yml` before CPack is allowed to run: a package is never
  built from a red tree.
