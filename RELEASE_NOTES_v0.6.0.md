# proJV v0.6.0 Release Notes

> Date: 2026-10-25
> Compare: `v0.5.6` -> `v0.6.0`
> Upgrade from any earlier build by replacing `proJV_tui` and `projv_files/`.
> No config migration needed.

---

## Highlights

### Skills scale to real projects

One driver reply can now call `request_agent` for several different workers and
they run **concurrently** (bounded by `max_parallel`, default 16). Calls to the
same worker stay ordered, every call still gets a tool result, and results are
written back in the order they were requested.

Dispatch is one layer deep by design: only an agent that appears in
`round_steps` / `on_converge` gets the `request_agent` tool. Leaf workers never
see it, and dispatching to a driver is refused explicitly.

### Monica is now 1 supervisor + 10 workers

Rewritten for 50-200 file projects: the supervisor inspects the tree, splits it
into batches of 3-4 coupled files, dispatches all ready batches in a single
reply, then balances "dispatched" against "reported" before it is allowed to
finish. Workers run with thinking off (the job is mechanical), share one
`worker.md`, and report in a fixed format.

Measured on this repository (94 C++ files, three runs): 94/94 files reported,
0 failed, 30 turns, 0 `NO_REPORT`, average 5.7-8.8 tool calls per turn against a
90-call budget. The first run, before the tuning, needed 6 rounds and left 4
batches unreported every round.

### Thinking mode is configurable per role

`thinking: "on" | "off"` at the skill level, overridable per agent. Default is
on, which is what the API does anyway. A forced `tool_choice` always disables
thinking, because the API rejects the two together; `bigbang` now runs in `auto`
mode and no longer pays for the forced-tool-call fallback.

### File tools that survive large files

| Tool | What is new |
|------|-------------|
| `read_file` | `start_line` / `end_line`. Range reads bypass the 100KB whole-file cap (2MB scan), return `[lines A-B of T]`, number every line, cap at 1000 lines / 100KB and name the next `start_line`. `path#L120-L480` works in skill `file_requests` |
| `list_dir` | new. Indented tree, `depth` 1-6, `exclude` regex, skips build/VCS directories, prints `(N entries, not expanded)` at the depth edge |
| `grep_files` | `max_results` (default 100, max 500), an explicit `[truncated at N results]` marker, and a single file path now searches **that file** instead of answering "no matches" |
| `format_file` | new. `mode="clang"` runs clang-format with the nearest `.clang-format`; `mode="whitespace"` is a dependency-free cleanup pass. `clang_format_path` is new in `config.toml` |

### Config errors are no longer silent

`tool_mode`, `converge`, `thinking`, `shape`, `emit`, `submit_reserve` and `when`
are validated, and an illegal value is reported with the legal set. Unknown keys
(including the removed `max_recursion`) become warnings through the skill event
stream instead of quietly falling back to a default. Actions that name an
undeclared agent fail loudly.

---

## Repository hygiene

### One house style

`.clang-format` (4 spaces, attached braces, 100 columns, includes not sorted,
`FixNamespaceComments: off`) plus a one-time reformat: 124 files, ~3500 lines.
Verified by stripping whitespace, quotes and comment markers from every file and
comparing byte-for-byte against the previous revision - identical everywhere. The
commit is listed in `.git-blame-ignore-revs`, so `git blame` skips it.

`theme_colors.h`'s X-macro table is wrapped in `// clang-format off`: without the
guard clang-format turns it into an unreadable cascading staircase.

### CI is green on Windows and Linux

`ci.yml` builds and tests on `windows-latest` + `ubuntu-latest`; `release.yml`
packages on a `v*` tag. Two Windows-only failures had to be fixed to get there,
both of which had never been seen because the project had no CI before:

- `config.cpp` assigned `toml::parse_file()`'s `parse_result`, which converts
  implicitly to `table&`, `table&&` and `const table&` - ambiguous under
  `/permissive-`, the default from VS2022 on (VS2019 does not enable it).
- SQLiteCpp's `SQLITECPP_RUN_CPPCHECK` / `RUN_CPPLINT` default to ON and are
  attached as ALL custom targets, so a third-party lint gate aborted our build
  (`error MSB8066`). Both are now forced OFF before `add_subdirectory`.

The Windows job also now tees the build log and emits `::error::` annotations on
failure, so a Windows-only break is readable from the checks API instead of a
bare "exit code 1".

### Dead code actually deleted

Everything below was verified unreachable by grepping `src/` **and** `tests/`
(the earlier audit only scanned `src/`, which is how test-only symbols got
flagged), then removed:

- `DeepSeekClient::isReady`, `getModel` (declarations only), `healthCheck` (44 lines, no caller)
- `Agent::setStatus`, `setError` (declarations only)
- `Session::stripLastToolCallsAndResults` (26 lines; `softStripLastToolCalls` is the live path)
- `ThemeManager::loadFromFile` (24 lines), `themeDir()` getter
- `models.h` `StreamChunk`, `ToolApproval`, `PendingToolCall`, `AgentStatus::pendingTool`
- `deepseek.cpp` `eventType` (assigned, never read), `ProcessConfig::inheritEnv` (write-only)
- `IProcessRunner::IsRunning` and both overrides
- `src/format_compat.h`, `src/platform_compat.h` (whole headers, zero includes; re-add if a non-MSVC port starts)

16 files, +15 / -176.

---

## Fixed

Found by running monica on this repository, then verified by hand:

- `md_file` was broken on Linux: the tool carried its own copy of the workspace
  path guard, including the hardcoded `\`. It now uses the shared
  `validateToolPath()`, and the guard exists exactly once.
- `edit_file` had **no** workspace validation at all - it only resolved relative
  paths against the current directory, so it could rewrite any file on disk. Now
  guarded like every other file tool.
- `file_search` had no workspace validation and no junk-directory filter (it
  walked `build/`, `node_modules/`, `.git/`), and treated `.` as a wildcard.
- `ProcessResult::stderr_` was never written on either platform, yet
  `python_tool_manager` read it and reported empty errors. POSIX now fills it
  (its pipe already existed); on Windows stderr shares the stdout pipe and the
  readers fall back to it.
- The Windows process runner ignored `ProcessConfig::extraEnv`, so pytool
  environment variables were silently dropped. Values are now quoted (`set "K=V"`).
- `isPathInWorkspace` rejected the workspace root itself, so `grep_files` and
  `list_dir` failed whenever they defaulted to it.
- In auto mode a model could answer in prose without calling the submission
  tool, and the turn was silently lost. `turn()` now nudges once ("call the
  '<verb>' tool"), and a turn that produces nothing returns
  `[no report] '<role>' …` instead of an empty string.
- `tests/CMakeLists.txt` applied `/utf-8` after `add_executable`, so it never
  reached the test binary.
- `exec_shell` no longer dereferences a null process runner;
  `SystemUtil::Instance()` throws with the broken contract instead of
  segfaulting.

## Known issues / deliberately not done

- **`clipboard` is implemented but not wired.** Nothing calls
  `clipboard::setText()`: the feature it was written for (copy the last answer
  from the TUI) has no key binding and no notion of a selected message. It is
  kept because the hard part - UTF-8 -> UTF-16 on Windows so CJK survives - is
  done, and the status is documented in `clipboard.h`. If it is wired up later,
  note that the Linux path shells out to `xclip`, which is not guaranteed to be
  installed.
- `ISystemUtil::GetSystemFontPath` / `InstallCrashHandler` /
  `GetPathSeparator` / `OpenUrl` are implemented on both platforms, used by
  nothing, and kept on purpose (removing them means removing their helpers too).
  Marked as such in the header.
- `theme_colors.cpp` assigns ~74 hex strings positionally; only the *count* is
  pinned by a test, so a reorder would silently mis-assign colours.
- The skill dead-code audit is name-based and not type-aware: a symbol it calls
  dead means "no textual caller in this repo", not "safe to delete".
- No extra demo GIFs ship in this release (`assets/raw/README.md` keeps the
  recording instructions). `assets/demo.gif` is the README hero;
  `assets/social-preview.png` is the link-preview card and must still be uploaded
  in Settings -> Social preview.

## Verification

- `proJV_tests`: **184 cases / 853 assertions** (839-849 on Linux, where the
  clang-format probe skips itself), no network required.
- Built and tested on Windows (MSVC, VS2019 locally; CI uses the runner image)
  and Linux (Ubuntu 22.04 / WSL2, g++ 11) - both green, in CI as well.
- `cpack` produces `proJV-0.6.0-windows-x64.zip` and
  `proJV-0.6.0-linux-x64.tar.gz`; both were unpacked and checked: only the
  binary, `projv_files/{fonts,pytool,skills,theme}`, READMEs and LICENSE - no
  `config.toml`, no sessions, no prompts.
- The reformat is provably whitespace-only (byte comparison with whitespace,
  quotes and comment markers removed); `clang-format --dry-run -Werror` reports
  all 126 files formatted.
- Real-machine acceptance: three `/monica` runs on this repository, 94 C++ files
  each, all files reported, zero deletions.

## Upgrade notes

- Monica's roles changed: `formatter` / `translator` / `auditor` are gone,
  replaced by `worker_1` .. `worker_10`. Replace the folder if you copied an
  older monica.
- Workers have no `exec_shell`. Formatting goes through `format_file`; install
  `clang-format` (or set `clang_format_path`) if you want real formatting rather
  than the whitespace fallback. Without a `.clang-format` in the project,
  `format_file` refuses in clang mode instead of guessing a style - that silence
  would have restyled a whole project to LLVM.
- `max_recursion` still loads, with a warning. `submit_reserve` is new and
  defaults to 0 (off), so existing skills behave exactly as before.
