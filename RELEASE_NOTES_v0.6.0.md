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

- `proJV_tests`: 170 test cases / 799 assertions, no network required.
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
- Still not verified: a full `/monica` run against a 200-file project with a
  real API key (see 05_SKILL_IMPROVE.md, acceptance section).
