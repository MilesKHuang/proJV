# monica -- a proJV skill (installable, self-contained)

MONICA is a supervisor-driven, multi-agent project tidy-up skill for proJV. It
is built for real projects: 50-200 files, not two.

## What it does
One supervisor plans the work, splits the project into batches of 6-10 coupled
files, and dispatches **several batches at once** to a pool of ten workers. The
workers run concurrently, each handling its own files: format, ASCII/full-width
cleanup, Chinese comments translated to English, and a read-only dead-code
audit. The supervisor verifies the reports and re-dispatches anything that
failed before it finishes with the consolidated report.

Dead code is only ever **reported** - a false "dead" is worse than a miss.

## Layout (this directory IS the repo)
    config.json        workflow definition (the DSL)
    supervisor.md      system prompt: the planner / dispatcher
    worker.md          system prompt: shared by worker_1 .. worker_10
    tools/emit.json    the one verb tool

## Why ten workers
The engine runs one concurrency group per target agent, so a single supervisor
reply that calls `request_agent` for ten different workers starts ten turns in
parallel (bounded by `max_parallel`). Workers never dispatch: dispatch is one
layer deep, by design. Workers run with `"thinking": "off"` because the job is
mechanical; the supervisor keeps its thinking chain.

Workers deliberately have no `exec_shell` (the process runner is a single
serial resource). They use `format_file`, which runs `clang-format` when the
machine has it and falls back to a built-in whitespace pass when it does not.

## Install
Copy (or symlink) this directory into the app's skills folder:

    <projv_files>/skills/monica/

`projv_files` sits next to the proJV executable.

## Run
    /skill list                 # should list monica
    /skill monica <project-root>

Then watch the supervisor dispatch batches to several workers per round. Press
Esc to stop; cancellation is checked between batches and between workers.

## Tuning
| Key | Meaning |
|-----|---------|
| `max_rounds` | dispatch waves. 8 rounds is enough for a few hundred files |
| `max_parallel` | how many workers may run at once (default 16) |
| `max_tool_iters` | tool-call budget per turn; raise it if a worker runs out mid-batch |
| `stop_when` | the supervisor's `DONE` summary stops the run |
| `tools` | add `exec_shell` only if you accept that workers then serialize |

## Notes / limits
- Without `clang-format` installed, formatting is whitespace-only (tabs,
  trailing spaces, final newline). Install it or set `clang_format_path` in
  `config.toml` for real formatting.
- Batches are disjoint by design; two workers editing one file is a bug in the
  dispatch plan, so the supervisor is told to never split a file across batches.
- A worker's report is capped at ~30 lines on purpose: ten workers per round
  must not flood the supervisor's context.
