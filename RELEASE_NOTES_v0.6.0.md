# proJV v0.6.0 Release Notes

> Date: 2026-10-25
> Compare: `v0.5.6` -> `v0.6.0`

---

## Highlights

### Skills scale to real projects

The skill engine now dispatches work **concurrently**. One driver reply can call
`request_agent` for several different workers, and those workers run in
parallel (bounded by `max_parallel`, default 16). Calls to the same worker stay
in order, every call still gets its tool result, and results are written back
in the order they were requested.

Dispatch is one layer deep by design: only an agent that appears in
`round_steps` / `on_converge` gets the `request_agent` tool. Leaf workers never
see it, and dispatching to a driver is refused explicitly.

### Monica is now 1 supervisor + 10 workers

`monica` was rewritten for 50-200 file projects: the supervisor inspects the
tree, splits it into batches of 6-10 coupled files, dispatches all ready
batches in a single reply, then verifies every report and re-dispatches
failures before it finishes. Workers run with thinking off (the job is
mechanical), share one `worker.md`, and report in a fixed format.

### Thinking mode is configurable per role

`thinking: "on" | "off"` at the skill level, overridable per agent. Default is
on, which is what the API does anyway, so nothing changes for existing skills.
A forced `tool_choice` always disables thinking, because the API rejects them
together; `bigbang` now runs in `auto` mode and no longer pays for the forced
tool-call fallback round trip.

### File tools that survive large files

| Tool | What is new |
|------|-------------|
| `read_file` | `start_line` / `end_line`. Range reads bypass the 100KB whole-file cap (2MB scan), return `[lines A-B of T]`, number every line, cap at 1000 lines / 100KB and tell you the next `start_line`. `path#L120-L480` works in skill `file_requests` |
| `list_dir` | new. Indented tree, `depth` 1-6, `exclude` regex, skips build/VCS directories, prints `(N entries, not expanded)` at the depth edge |
| `grep_files` | `max_results` (default 100, max 500) and an explicit `[truncated at N results]` marker, so an incomplete search can never be mistaken for "no callers" |
| `format_file` | new. `mode="clang"` runs `clang-format` (honours the nearest `.clang-format`, 30s timeout, clear error when the binary is missing); `mode="whitespace"` is a dependency-free cleanup pass. `clang_format_path` is new in `config.toml` |

### Config errors are no longer silent

`tool_mode`, `converge`, `thinking`, `shape`, `emit` and `when` are validated,
and an illegal value is reported with the legal set. Unknown keys (including
the removed `max_recursion`) are reported as warnings through the skill event
stream instead of quietly falling back to a default. Actions that name an
undeclared agent now fail loudly.

### Smaller fixes

- `read_file` / `list_dir` / `grep_files` used to reject the workspace root
  itself, so the default path always failed; fixed.
- In auto mode the model sometimes answered in prose without calling the
  submission tool. The turn now nudges it once ("call the '<verb>' tool")
  before giving up, so a skill can no longer lose a worker's report.
- `tests/CMakeLists.txt` applied `/utf-8` after `add_executable`, which never
  reached the test binary; now target-scoped.

---

## Follow-up: what running monica on this repository found

The first real run (`/monica` on `src/`, 94 files, 10 workers) processed every
file but exposed four problems. All are fixed here.

**Bugs found in the code**

- `md_file` was broken on Linux: the tool carried its own copy of the workspace
  path guard, complete with the hardcoded `\`. It now uses the shared
  `validateToolPath()` (-76 lines of duplicated logic).
- `grep_files` answered `No matches found` when given a single file path, because
  a file went into a recursive directory walk. That false negative is exactly
  what corrupts a dead-code audit. It now searches that one file.
- The Windows process runner ignored `ProcessConfig::extraEnv`, so pytool
  environment variables were silently dropped there.

**Skill tuning, driven by the numbers**

Round 1 dispatched 86 files and 18 reported; shrinking batches to 1-3 files in
round 6 made it 3/3. The per-file workload, not the model, was the problem.

- `worker.md`: the dead-code audit used one `grep_files` per symbol (20 symbols
  x 8 files against a 60-call budget = guaranteed death). It is now one
  alternation regex per file, with a hard cap of ~12 calls per file, an explicit
  budget section, an ASCII/keep-me table (two workers disagreed about em dashes
  vs UI glyphs) and report fields that separate "audited and clean" from "not
  audited".
- `supervisor.md`: batches are 3-4 files, failures are re-dispatched smaller, and
  it must balance "dispatched files" against "reported files" before `DONE`. It
  also may no longer claim a change count it cannot see.
- `config.json`: `max_tool_iters` 60 -> 90, `max_rounds` 8 -> 10.

**Engine contract**

- A turn never returns an empty string any more. It returns
  `[no report] '<role>' ran out of tool budget (N calls) without calling '<verb>'`.
  Previously silence propagated up and the batch looked like it never existed.
- `submit_reserve` (new, default 0 = off): say "stop exploring, submit now" once
  the last N iterations are reached. Soft on purpose - forcing `tool_choice`
  would disable thinking, which `bigbang` relies on. At 0 the request sequence is
  unchanged, so existing skills are unaffected.

---

## Upgrade notes

- No config migration needed. `max_recursion` still loads, with a warning.
- Monica's roles changed: `formatter` / `translator` / `auditor` are gone,
  replaced by `worker_1` .. `worker_10`. If you copied monica before, replace
  the folder.
- Workers have no `exec_shell`. Formatting goes through `format_file`; install
  `clang-format` (or set `clang_format_path`) to get real formatting instead of
  the whitespace fallback.

---

## Verification

- `proJV_tests`: 179 test cases / 829 assertions, no network required.
- Built and tested on **two platforms**: Windows (MSVC 19.29, VS 2019) and
  Linux (Ubuntu 22.04 / WSL2, g++ 11, CMake 3.22) - both green.
- `cpack` produces `proJV-0.6.0-windows-x64.zip` and
  `proJV-0.6.0-linux-x64.tar.gz`; both archives were unpacked and checked: they
  carry only the binary, `projv_files/{fonts,pytool,skills,theme}`, the READMEs
  and the LICENSE - no `config.toml`, no sessions, no prompts.
- New suites: `test_file_tools.cpp`, `test_skill_thinking.cpp`,
  `test_skill_parallel.cpp`, plus a rewritten `test_skill_monica.cpp`.
- Found by the Linux run: `isPathInWorkspace` appended a Windows separator, so
  every file tool rejected sub-paths of the workspace on POSIX. Rewritten as a
  component-wise path comparison (separator agnostic, still case-insensitive on
  Windows). 12 test cases caught it.
- Verified by a real `/monica` run on this repository (94 files). A second run with the tuned skill is the next step; the token/call counts from the first run were not recorded.
