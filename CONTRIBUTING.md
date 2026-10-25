# Contributing to proJV

[简体中文](CONTRIBUTING_zh.md) | **English**

Thanks for taking a look. This file is a signpost, not a manual: the detail
lives in the dev guide.

## Before you start

1. Read [docs/en/dev-guide/README.md](docs/en/dev-guide/README.md) — request
   path, build commands, code conventions (threading, cancellation, error
   format, test seams).
2. Skim [module-map.md](docs/en/dev-guide/module-map.md) to find the module your
   change belongs to.
3. Find your change in [recipes.md](docs/en/dev-guide/recipes.md) — it lists the
   files to touch, in order.

## Build and test

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Windows: run it from an x64 Native Tools prompt (Visual Studio 2019+).
Linux: `sudo apt-get install -y libcurl4-openssl-dev` first.

A PR is only mergeable when the full `proJV_tests` suite passes. New behaviour
needs a new test; a bug fix needs a test that fails before it.

## Branches and commits

- Branch from `main`: `feat/<short-slug>`, `fix/<short-slug>`,
  `docs/<short-slug>`.
- Commit subjects follow `type(scope): summary` — `feat(tools)`, `fix(skill)`,
  `docs:`, `build:`, `chore(tests)`.
- In the body, say **why** and list the files that changed shape (new config
  keys, new tools, new seams). Reviewers read the body, not the diff.

## Pull requests

Checklist in [.github/PULL_REQUEST_TEMPLATE.md](.github/PULL_REQUEST_TEMPLATE.md):

- tests pass locally;
- new or changed behaviour is covered by a test;
- **docs updated in both languages** — `docs/en/` and `docs/zh/` are mirrors,
  same file names; `README.md` and `README_zh.md` likewise;
- no secrets: never commit `config.toml`, `projv_files/sessions/`, API keys or
  logs.

## Things that will be declined

- Turning the single process runner into a pool, or dropping its lock. It is
  serial on purpose (see `src/platform/iprocess_runner.h`).
- Adding an MCP layer, a plugin registry, or a package manager.
- Hardcoding colours outside the theme system.
- A skill that deletes code it decided was dead. Dead code is reported, never
  removed.

## Reporting bugs

Use the issue template; include the proJV version (status bar / `/help`), OS and
terminal, model name, steps to reproduce and the relevant slice of
`projv_files/logs`. Scrub your API key.
