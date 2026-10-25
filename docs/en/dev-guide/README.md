# proJV developer guide

> After this page you can build it, run the tests, and know which file to open
> for a given change.

## Request path

```
input box
   |  Agent::submitUserMessage
   v
Agent (src/core/agent.*)  -- session, context budget, compaction
   |  ChatRequest
   v
DeepSeekClient (src/client/deepseek.*)  -- libcurl, SSE, retries
   |  StreamCallbacks: text / thinking / tool_call
   v
Agent turn loop  -- approve? -> ToolRegistry::execute -> tool result
   |                      (or: skill phase, see below)
   v
ChatView / bubbles (src/tui/*)  <-- onEvent / onProgress
```

A skill is its own phase: after the tool phase ends, `TuiApp::runSkillBlocking`
constructs a `SkillRunner`, which owns a pool of `SubAgent`s. Each `SubAgent`
runs its own LLM loop; the runner interprets `config.json` (rounds, actions,
slots, votes) and emits `[Role]` messages into the same chat.

## Build and test

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

- Tests are doctest; `tests/unit/` is logic, `tests/golden/` renders FTXUI
  screens in memory (no terminal needed).
- `PROJV_BUILD_TESTS=ON` is the default; a release build ships no tests.
- CI (`.github/workflows/ci.yml`) runs the same three commands on
  `windows-latest` + `ubuntu-latest`; a `v*` tag runs `release.yml` instead,
  which also packages with CPack.

## Directory tree

```
src/
├── tui/        entry point, app state, chat view, markdown, theme, status, todo
├── core/       agent, session, config, storage, prompts, project context,
│               skill engine (skill_runner / sub_agent / skill_config)
├── client/     DeepSeek API client, request/response models
├── tools/      registry + one file per built-in tool, web tools, pytool manager
└── platform/   process runner and system utilities (Windows / Linux)
tests/          doctest unit tests + golden render tests
skills/         example skills (bigbang, monica) deployed next to the binary
projv_files/    runtime data: fonts, theme, pytool, skills (config/prompts/
                sessions are created at first launch and never packaged)
external/       vendored deps: FTXUI, SQLiteCpp, curl, json, toml++, loguru, doctest
```

## Dependencies

| Library | Purpose | Where |
|---------|---------|-------|
| FTXUI 7.0.3 | TUI | `external/` |
| SQLiteCpp 3.3.3 | sessions | `external/` |
| libcurl 8.21.0 | HTTP | `external/` on Windows, system on Linux |
| nlohmann/json | JSON | single header |
| toml++ | config.toml | single header |
| loguru | logging | `external/loguru.cpp` |
| doctest | tests | single header |

No vcpkg / conan / npm / pip.

## Code conventions that actually matter

- **Threading.** The TUI thread owns FTXUI. Long work (API, tools, skills) runs
  on worker threads. UI callbacks (`onEvent`, `onProgress`) are called from the
  runner thread only after a join, never from a worker thread.
- **Cancellation.** `Esc` -> `Agent::cancel()` -> `ToolRegistry::cancelAll()` ->
  `IProcessRunner::Cancel()`; skills additionally use a cancel probe
  (`SkillRunner::setCancelProbe`) because the runner cannot be reached from the
  TUI directly.
- **One process runner.** `Run()` serializes the whole child-process lifetime on
  purpose. Do not turn it into a pool; concurrent workers must use in-process
  tools. See the note in `src/platform/iprocess_runner.h`.
- **Error format.** Tools return strings; errors start with `Error:` (or
  `[error]` inside skills) and are meant to be read by the model.
- **Test seam.** `SubAgent` accepts scripted responders
  (`setResponder` / `setResponderMulti` / `setResponderText`), so skill tests
  never touch the network.
- **Encoding.** Sources are UTF-8; MSVC builds need `/utf-8` (already set per
  target).

---

Read next -> [module map](module-map.md) | Recipes -> [recipes](recipes.md)
