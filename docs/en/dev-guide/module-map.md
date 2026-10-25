# Module map

A route card per module: what it does, where it starts, what to read next to it,
and which test covers it. File names are the source of truth - if this page and
the code disagree, the code wins.

---

## 1. Entry and TUI shell

- **Responsibility.** Own the terminal, the app state machine and the event loop.
- **Entry.** `src/tui/main_tui.cpp` -> `TuiApp::setup()` and `run()` in
  `src/tui/app_tui.cpp`. `setupTools()` is where every tool gets registered.
- **Data flow.** key event -> `TuiApp` -> agent or skill; agent callbacks ->
  `ChatView` bubbles + status bar.
- **Read together with.** `chat_view.*`, `status_bar.*`, `status_line.*`,
  `approval_logic.*`, `config_view.*`, `skill_trigger.*`.
- **Tests.** `tests/golden/*`, `test_approval_logic.cpp`, `test_skill_trigger.cpp`.

## 2. Markdown rendering

- **Responsibility.** Turn streamed text into coloured, wrapped lines.
- **Entry.** `src/tui/markdown_text.cpp` (tokenise/wrap) and
  `src/tui/markdown_view.cpp` (FTXUI elements), `bubble_model.cpp` for bubbles.
- **Read together with.** `theme_colors.*` (colours come from the theme).
- **Tests.** `test_markdown_text.cpp`, `test_bubble_model.cpp`,
  `golden/test_markdown_render.cpp`.

## 3. Theme

- **Responsibility.** Load/save theme JSON, map semantic roles to colours,
  drive per-speaker colours.
- **Entry.** `src/tui/theme_manager.cpp`, `theme_map.cpp`, `theme_colors.cpp`,
  `speaker_colors.cpp`, editor in `theme_editor_view.cpp`.
- **Tests.** `test_theme_*`, `test_speaker_colors.cpp`, `golden/test_theme_editor.cpp`.

## 4. Agent core

- **Responsibility.** The turn loop: build the request, stream, approve and run
  tools, persist everything, compact when context is tight.
- **Entry.** `src/core/agent.cpp` (`submitUserMessage`, tool phase, turn end),
  `session.cpp` (context window + DB), `prompts_loader.cpp` (roles),
  `project_context.cpp` (workspace snapshot injected into the prompt).
- **Data flow.** `AppConfig` + `Session` -> `ChatRequest` -> callbacks ->
  `Storage` (SQLite) + UI.
- **Read together with.** `client/models.h` (request/response types),
  `tools/registry.h`, `config.*`.
- **Tests.** `test_agent_request.cpp`, `test_turn_end.cpp`,
  `test_reasoning_content.cpp`, `test_prompt_*`.

## 5. API client

- **Responsibility.** Serialize a request, stream SSE, retry, cancel, and format
  the official Search / Fetch responses.
- **Entry.** `src/client/deepseek.cpp` (`buildRequestBody`, `streamBlocking`,
  `parseSSEChunk`), types in `models.h` / `models.cpp`.
- **Gotchas.** Assistant messages must always carry `reasoning_content` when
  `tools` are present; a forced `tool_choice` is incompatible with thinking mode
  (proJV disables thinking in that case).
- **Tests.** `test_deepseek_sse.cpp`, `test_reasoning_content.cpp`,
  `test_image_input.cpp`.

## 6. Tools

- **Responsibility.** One function per capability, exposed through a registry
  with JSON schemas.
- **Entry.** `src/tools/registry.cpp`; `file_tool.cpp` (read/write/list/grep),
  `edit_file_tool.cpp`, `md_file_tool.cpp`, `file_search_tool.cpp`,
  `format_tool.cpp`, `shell_tool.cpp`, `web_tools.cpp`, `todo_tool.cpp`,
  `python_tool_manager.cpp`.
- **Rule.** Every path goes through `validateToolPath` (`file_tool.h`); a tool
  never touches the filesystem outside the workspace.
- **Tests.** `test_file_tools.cpp`, `test_web_api.cpp`.

## 7. Skill engine

- **Responsibility.** Run a JSON-described multi-agent program: fixed agents,
  rounds, parallel actions, slots/votes, convergence, event emission.
- **Entry.** `src/core/skill_runner.cpp` (`run`, `executeSteps`, `mergeAction`,
  `handleRequestAgent`), `sub_agent.cpp` (one LLM loop per role, tool
  dispatch), `skill_config.h` (DSL + validation), `skills_builtin.cpp`
  (self-seeding `skill_maker`).
- **Concurrency.** One reply may contain several `request_agent` calls: they are
  grouped by target, groups run concurrently (bounded by `max_parallel`), each
  target stays serial, results are written back in call order. Dispatch is one
  layer deep: only agents in `round_steps` / `on_converge` get the tool.
- **Read together with.** `tools/registry.h`, `client/deepseek.h`,
  `skills/*/config.json`.
- **Tests.** `test_skill_runner.cpp`, `test_skill_engine.cpp`,
  `test_skill_swarm.cpp`, `test_skill_parallel.cpp`, `test_skill_thinking.cpp`,
  `test_skill_monica.cpp`, `test_skill_maker.cpp`.

## 8. Storage and config

- **Responsibility.** `config.toml` round-trip and SQLite session persistence.
- **Entry.** `src/core/config.cpp` (TOML load/save, defaults),
  `src/core/storage.cpp` (DB create/open, message append, checkpoint).
- **Tests.** covered indirectly by every skill/agent test; no dedicated suite.

## 9. Platform

- **Responsibility.** Hide Windows vs POSIX differences.
- **Entry.** `src/platform/iprocess_runner.h` + `process_runner_win.cpp` /
  `process_runner_linux.cpp`, `system_util_*`, `isystem_util.h`.
- **Rule.** One runner, serialized on purpose (see the comment in the header).

---

Back -> [dev guide](README.md) | Recipes -> [recipes](recipes.md)
