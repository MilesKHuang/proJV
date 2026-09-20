# monica -- a proJV skill (installable, self-contained)

MONICA is a supervisor-driven, multi-agent project tidy-up skill for proJV.

## What it does
A supervisor agent plans batches of files and dispatches three workers via
`request_agent`: `formatter` (run the project formatter + strip full-width
chars), `translator` (Chinese comments -> English), and `auditor` (scan for
likely dead code -- **report only, never deletes**). The supervisor loops until
it reports `DONE` (see `stop_when` in `config.json`).

## Layout (this directory IS the repo)
    config.json        workflow definition (the DSL)
    supervisor.md      system prompt: the planner
    formatter.md       worker prompt
    translator.md      worker prompt
    auditor.md         worker prompt
    tools/emit.json    the one verb tool

## Install
Copy (or symlink) this directory into the app's skills folder:

    <projv_files>/skills/monica/

`projv_files` sits next to the proJV executable.

## Run
    /skill list                 # should list monica
    /skill monica <project-root>

Then watch the supervisor dispatch workers round by round. Press Esc to stop.

## Notes / limits
- Dead code is only ever **reported** (a false "dead" is worse than a miss).
- Batches are small by design; a large repo takes many rounds (`max_rounds`).
