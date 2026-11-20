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

Linux (needs `libcurl4-openssl-dev` and, for the GUI, `libsdl2-dev`):

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
- Every extension is a file: skills, roles, themes, pytool folders, prompt files, and MCP
  servers declared in `projv_files/mcp.json`. MCP servers are external processes spoken to over
  stdio JSON-RPC -- never loaded as code, never registered through a plugin registry, never
  installed through a package manager. Do not add a plugin registry, a package manager, or
  anything the binary loads as code.
- Do not introduce colors outside the theme system. A front-end may only read
  `ThemeColors` (`src/tui/theme_colors.h`) or the theme JSON in `projv_files/theme/`;
  each front-end owns its own conversion (`theme_map.cpp` for FTXUI, the studio's own
  for ImGuiCol). A new visual element needs a theme field, never a hardcoded RGB.
- The two front-ends must not diverge. Anything the user reads -- a label, a colour
  reference, a derived summary, a row order -- comes from `src/core/view_model.h`,
  not from the front-end's own copy. `proJV_lib` stays front-end neutral: no FTXUI
  and no ImGui type in it, and imgui/SDL2 only appear in `studio/`. A change that
  alters what is on screen needs `tests/unit/test_view_model.cpp` to agree.
- Do not let a skill write outside the tool roots. Skills may write the workspace and
  `projv_files` (prompts, skills, theme, pytool, `mcp.json`) -- the same roots as the main
  agent, enforced by `ToolPaths` -- but must report which files they changed, and never touch
  `projv_files/sessions/` or `projv_files/logs/` (read-only). A skill's own `config.json`
  decides which tools it gets; a skill with `exec_shell` has the full power of the shell.
- Prompt text is not a documentation site. The per-turn capability map
  (`src/core/context_assembler.cpp`) is the only list of tools the model reads: do not
  re-add a tool inventory to `prompts/*.md`, and never let the seed contain the allowlist
  trigger literally (`parseAvailableTools` would read the prose as a tool list).
- Do not change the version anywhere except `project(proJV VERSION ...)` in `CMakeLists.txt`.
- Do not modify `external/` (vendored third-party code). A new vendored directory is
  allowed only by adding a row to the "Vendored dependencies" table below; once it
  lands it is read-only -- local fixes go upstream, not into the copy.

## Vendored dependencies

One row per directory in `external/`: version, license, and the target that needs it.
A directory without a row is a review rejection.

| Directory | Version | License | Used by |
|---|---|---|---|
| `FTXUI-7.0.3` | 7.0.3 | MIT | `proJV_tui` |
| `imgui-1.93.0-docking` | 1.93.0 (docking branch) | MIT | `proJV_studio` |
| `SQLiteCpp-3.3.3` | 3.3.3 | MIT | `proJV_lib` |
| `curl-8.21.0` | 8.21.0 | curl | `proJV_lib` (Windows only) |

`proJV_studio`'s Linux half links the SYSTEM SDL2 (`libsdl2-dev`), the same
treatment libcurl gets there, so SDL2 is not a vendored directory.

## Docs and commits

- Every user-facing change lands in both `docs/en/` and `docs/zh/` (same for `README.md` and
  `README_zh.md`). A one-language change is incomplete.
- Never commit an API key, `config.toml`, `projv_files/sessions/` or logs.
- `CONTRIBUTING.md` / `CONTRIBUTING_zh.md` stay the full human-facing guide; this file is the
  short version for agents. When build steps or prohibitions change there, change them here too.
- `docs/archive/` is gitignored design notes: never commit it (no `git add -f`,
  no `!` exception) and never link it from a committed file (`README*`, code
  comments, release notes, CMake comments). Rationale that must be public goes
  to `docs/en/` + `docs/zh/`.
