# v0.6.6 - one role, and skills that work on any subject

## One role prompt instead of three

`coder.md`, `designer.md` and `analyzer.md` were three files carrying three
different sets of rules, and the split only bought a narrower tool list: "plan
first, then execute" fits in the TODO list, not in a second prompt file. They are
replaced by a single seeded `assistant.md`.

- The hard rules that only one of the three carried are now in all of them:
  8KB write cap, `timeout_ms`, batched tool calls, `run_skill` / `view_image`
  discipline, `md_file`-only documents with the five-section structure,
  thread-safety and resource-lifetime checks, diagram guidelines, English code.
- The tool line grows to 15. `list_dir` (planning) and `format_file` (end of an
  edit) were registered but unreachable from every role.
- Planning is `update_todo(init)` plus a confirm gate, execution is
  `update_todo(update)`, closing is `update_todo(done)`. No phase switching.
- The expression rule is judgeable instead of a tone hint: lead with the
  conclusion in language a new graduate can follow, then the details - and match
  the language of the session and the project instead of hard-coding Chinese.
- **No role name is reserved.** A `.md` file in `projv_files/prompts/` is a role
  and the file name is the role name; only `compactor.md` is special (it is the
  `/compress` prompt). Installs that still carry the three old files keep them
  as ordinary roles - delete the file to drop the role.

## Skills work on any subject

`bigbang` and `avenger` both assumed software and a founder.

- **bigbang** accepts a source fetched this round, not only a file read this
  round, may search the web, reads "file path / class / interface" as
  "part / step / material / person", and opens every proposal with five plain
  lines. It also writes its plan to `docs/sop/bigbang-<slug>.md` - until now the
  document existed only in the chat session.
- **avenger** gains `research_aud`: round 1 learns the field from introductory
  material and writes a knowledge map (vocabulary, key questions, the *type* of
  source each line should hunt, the local unknowns no search can settle). Stark
  dispatches from that map, the one-person path is derived from the map instead
  of a fixed template, and every report opens with plain lines.

Both were run end to end on non-software topics before this release: a bakery
opening plan (bigbang) and a monster AI behaviour tree (avenger, nine agents,
one knowledge map, four field reports).

## OSC 7501 rev 0.4 compliance

- C1 controls (U+0080-U+009F) are filtered by codepoint. A byte-wise check let
  the two-byte UTF-8 form through, which made the host drop the whole record.
- `app` is on every record, `clear` included: the clear branch returned before
  the field was appended.
- An interrupted exit (Ctrl-C, window close) leaves `error` instead of silently
  clearing, so the run stays visible in the host's list. A normal exit still
  clears.

## Housekeeping

- `src/core` was formatted with the repository's `.clang-format`; formatting
  only, no semantic change.
- `docs/sop/` - the directory the research skills write to - is ignored.
- Docs rewritten in both languages: `03-roles.md` (one role, and the honest
  statement that the `tools available:` line is a permission table, not a
  preference hint) and `06-skills.md` (both skills on any subject, avenger's
  reconnaissance round, where each skill writes its output).

## Verification

- `cmake --build build --config Release` clean on MSVC 19.29 (Windows).
- `ctest --test-dir build -C Release --output-on-failure`: 285/285 cases,
  1481 assertions, 0 failed.
- New tests: `test_prompt_seed` (tool line, expression rule, no legacy role
  names, `defaultPromptIndex`), the C1/C2/C3 cases in `test_osc7501`, and the
  golden-bytes case in `test_skill_runner`, which moved with bigbang's new final
  instruction.
- CI runs the same suite on Windows and Ubuntu for every push to `main`, and
  again inside `release.yml` before CPack is allowed to run.
