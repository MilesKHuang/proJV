# AGENTS.md

proJV is a native C++20 terminal agent for the DeepSeek API: one binary, deps vendored
under `external/`. Architecture and module walkthroughs live in `docs/en/dev-guide/`
(mirror: `docs/zh/dev-guide/`); this file only carries what you cannot infer from code.

## Build and test

Windows (MSVC 2019 or newer, plain cmd prompt):

    call "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
    cmake -S . -B build -G "Visual Studio 16 2019" -A x64
    cmake --build build --config Release --parallel
    ctest --test-dir build -C Release --output-on-failure

Linux (needs `libcurl4-openssl-dev`):

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --parallel
    ctest --test-dir build --output-on-failure

- Changing code means building AND running the whole suite: CI runs the same `ctest` on
  every push and tag, so a red build is a broken release.
- New behaviour needs a test in `tests/unit/`, and the file must be added to the explicit
  `TEST_SOURCES` list in `tests/CMakeLists.txt` (it is not a glob).
- First Windows build compiles libcurl and FTXUI from source: expect 15-25 minutes.
- Paths that need a live model (the `view_image` round, `/clear`, `F3` context injection) have
  a headless harness: build with `-DPROJV_BUILD_E2E=ON`, then run
  `powershell -NoProfile -ExecutionPolicy Bypass -File tests\e2e\run_e2e.ps1`. It drives the
  real `TuiApp` against `tests/e2e/fake_server.py` and prints one PASS/FAIL line per check.

## Do not

- Do not turn `IProcessRunner` into a pool or drop its lock. One child process at a time is
  the design; the reasoning is written out in `src/platform/iprocess_runner.h`.
- Do not add an MCP layer, a plugin registry or a package manager. Every extension is a file:
  skills, roles, themes, pytool folders, prompt files.
- Do not introduce colors outside the theme system (`src/tui/theme_*`).
- Do not let a skill write or delete repository files. Skills report; the agent edits.
- Do not change the version anywhere except `project(proJV VERSION ...)` in `CMakeLists.txt`.
- Do not modify `external/` (vendored third-party code).

## Docs and commits

- Every user-facing change lands in both `docs/en/` and `docs/zh/` (same for `README.md` and
  `README_zh.md`). A one-language change is incomplete.
- Never commit an API key, `config.toml`, `projv_files/sessions/` or logs.
- `CONTRIBUTING.md` / `CONTRIBUTING_zh.md` stay the full human-facing guide; this file is the
  short version for agents. When build steps or prohibitions change there, change them here too.
