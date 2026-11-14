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
- **Fixed context injection.** The directory tree and `AGENTS.md` both come from
  `TuiApp::injectWorkspaceContext()`; startup, `F3` and `/clear` share it (`/clear`
  goes through `reinitConversation(true)` and keeps the session file). The dynamic
  todo/overview messages are inserted by `Agent::buildChatRequest()` right after
  the fixed system block, so the prefix cache stays stable.
- **Read together with.** `chat_view.*`, `status_bar.*`, `status_line.*`,
  `approval_logic.*`, `config_view.*`, `skill_trigger.*`, `core/turn_requests.h`.
- **Slash menu.** `src/tui/slash_menu.{h,cpp}` is the `/` completion list: a pure
  table (builtins + installed skills, description read from `config.json`) plus a
  prefix-then-substring filter. `main_tui.cpp` drives it from `InputOption::on_change`
  and hijacks Up/Down/Enter/Escape in the existing `CatchEvent` while it is open.
  It only fills the input box; nothing in the execution path changes.
- **Background dock.** The right dock shows skill agents and running `bg#N` tasks
  (`TaskRegistry::snapshots()`), each with its latest output line. The line is the
  LAST non-empty line, not the first: a narration is a paragraph and the first line
  is its oldest sentence.
- **Tests.** `tests/golden/*`, `test_approval_logic.cpp`, `test_skill_trigger.cpp`,
  `test_agents_md.cpp`, `test_turn_end.cpp`.

## 2. Markdown rendering

- **Responsibility.** Turn streamed text into coloured, wrapped lines.
- **Entry.** `src/tui/markdown_text.cpp` (tokenise/wrap) and
  `src/tui/markdown_view.cpp` (FTXUI elements), `bubble_model.cpp` for bubbles.
- **Diff bubble.** `src/tui/diff_view.cpp` re-projects the stored unified diff
  into two panes (old line numbers | new line numbers, changes paired on one
  row) when the chat column is at least `kSplitMinWidth` wide, and into the
  single-column block otherwise. Parse and render only: the database stores the
  unified diff and nothing more, so old sessions replay in the new layout.
- **Read together with.** `theme_colors.*` (colours come from the theme);
  `chat_view.cpp` passes the measured chat-column width in.
- **Tests.** `test_markdown_text.cpp`, `test_bubble_model.cpp`,
  `test_diff_parse.cpp`, `golden/test_markdown_render.cpp`,
  `golden/test_chat_render.cpp`.

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
  `context_assembler.{h,cpp}` (P1 environment, P3 capability map),
  `project_context.cpp` (P2: directory tree + AGENTS.md in one message).
- **Message layout.** Fixed prefix `[P0]` role, `[P1]` environment, `[P2]` project --
  three persisted system messages, always three: P1 and P2 write placeholders instead
  of disappearing, so the count cannot drift. `[P3]` capabilities is rebuilt every
  turn by the TUI right after `McpManager::syncTools()` and handed to the Agent as
  opaque text, so the map names exactly the tools the request carries. The per-turn
  dynamic block (user request, TODO, overview) goes after the prefix, and
  `Session::applyCompaction()` inserts its summary at the same index.
- **Why P3 is not persisted.** MCP servers register at a turn boundary and skills can
  be written at any moment, so a stored copy is stale by construction. Identical input
  produces identical bytes (tool names are sorted, servers and skills sorted too),
  which keeps the KV prefix cache hitting until the tool table really changes.
- **Data flow.** `AppConfig` + `Session` -> `ChatRequest` -> callbacks ->
  `Storage` (SQLite) + UI.
- **Read together with.** `client/models.h` (request/response types),
  `tools/registry.h`, `config.*`.
- **Tests.** `test_agent_request.cpp`, `test_turn_end.cpp`,
  `test_reasoning_content.cpp`, `test_prompt_*`.
- **Wish mode.** `src/core/wish.h` is the whole state (objective, passes, idle
  rounds, decisions taken, blocked). `wish_decide` / `wish_block`
  (`src/tools/wish_tool.cpp`) are always registered and filtered out of the request
  by `Agent::getFilteredToolDefinitions()` whenever no wish is active -- registering
  and unregistering at runtime would write `ToolRegistry`'s bare maps from a second
  thread. `TuiApp::runAgentTurnLoop` keeps looping while the wish is active and the
  TODO is open, and stops on: TODO empty (after one was seen), two idle passes, the
  model's `wish_block`, 50 passes, or `/wish cancel`.
- **Tests.** `test_agent_request.cpp`, `test_turn_end.cpp`,
  `test_reasoning_content.cpp`, `test_prompt_*`, `test_wish.cpp`.

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
  `python_tool_manager.cpp`, `tool_paths.{h,cpp}` (the permission matrix).
- **Rule.** Every path goes through `validateToolPath` (`file_tool.h`), which asks
  `ToolPaths::resolve` (`tool_paths.h`). The roots are: the workspace (read+write),
  `projv_files` (read+write), `projv_files/sessions` and `projv_files/logs` (read
  only). The deepest matching root decides, so the two read-only sub-roots sit
  inside the writable one. `forWrite` is a parameter, not a convention: a read-only
  root refuses `write_file` / `edit_file` / `md_file` / `format_file` with the same
  `__WORKSPACE_VIOLATION__:` marker `Agent::run()` matches, and the message lists
  every root so a retry goes somewhere legal.
- **Wiring.** `TuiApp::setupTools()` builds one `ToolPaths::forSession(workspace,
  projv_files)` and hands it to all six path-taking register functions. Tests pass
  `ToolPaths(root)` (implicit single writable root).
- **Diff capture.** The four writing tools snapshot the file around their own
  write and call `diff_capture::record` (`diff_capture.{h,cpp}`). The slot is
  `thread_local`, not a registry member: skill workers share one registry across
  parallel threads. `Agent::executeTool` resets it before the call and takes it
  after, then hangs it on the tool message. Display-only -- it is persisted with
  the row and never serialized.
- **Tests.** `test_file_tools.cpp`, `test_diff_capture.cpp`, `test_web_api.cpp`.
- **Output shaping.** `src/tools/shell_output.{h,cpp}` is the single place that
  truncates command output, and it keeps the TAIL: compilers and test runners print
  the verdict last. Two layers use it -- the platform runner keeps 4 MB (a memory
  guard, dropping the middle) and `exec_shell` keeps the last 400 lines, or 200 KB
  under `full_output`. Cuts back off to a UTF-8 boundary.
- **Background tasks.** `src/core/task_registry.{h,cpp}` spawns detached children
  that write straight to a per-task temp file -- a pipe nobody drains deadlocks the
  child once the OS buffer fills. One monitor thread per registry (not per task),
  joined in `shutdown()`; Windows adds a Job Object with `KILL_ON_JOB_CLOSE` so a
  proJV crash takes the children with it. `exec_shell(background:true)` starts one,
  `task_output` reads it, `/end_task` kills it, and the turn loop reports finished
  ids through `turn_requests` (`Kind::Process`).
## 7. Skill engine

- **Responsibility.** Run a JSON-described multi-agent program: fixed agents,
  rounds, parallel actions, slots/votes, convergence, event emission.
- **Entry.** `src/core/skill_runner.cpp` (`run`, `executeSteps`, `mergeAction`,
  `handleRequestAgent`), `sub_agent.cpp` (one LLM loop per role, tool
  dispatch), `skill_config.h` (DSL + validation). Nothing is compiled in and
  seeded: a skill is a folder the user (or the agent) writes.
- **Concurrency.** One reply may contain several `request_agent` calls: they are
  grouped by target, groups run concurrently (bounded by `max_parallel`), each
  target stays serial, results are written back in call order. Dispatch is one
  layer deep: only agents in `round_steps` / `on_converge` get the tool.
- **Read together with.** `tools/registry.h`, `client/deepseek.h`,
  `skills/*/config.json`, `skills/_authoring/skill-authoring.md` (the shipped
  authoring guide; no `config.json`, so `listSkills()` never lists it).
- **Tests.** `test_skill_runner.cpp`, `test_skill_engine.cpp`,
  `test_skill_swarm.cpp`, `test_skill_parallel.cpp`, `test_skill_thinking.cpp`,
  `test_skill_monica.cpp`.

## 8. Storage and config

- **Responsibility.** `config.toml` round-trip and SQLite session persistence.
- **Entry.** `src/core/config.cpp` (TOML load/save, defaults),
  `src/core/storage.cpp` (DB create/open, message append, checkpoint).
- **Schema.** `messages` carries a `diff TEXT` column: the unified diff a
  file-writing tool produced, shown in its own TUI bubble. It is display-only and
  must never reach `to_json`. Older databases get it through the same
  try-`ALTER TABLE` block as `reasoning_content` / `image_paths` / `synthetic`.
- **Tests.** `test_storage_diff.cpp` covers the `diff` round trip and the
  migration from a pre-`diff` schema; everything else is covered indirectly by
  the skill and agent suites.

## 9. Platform

- **Responsibility.** Hide Windows vs POSIX differences.
- **Entry.** `src/platform/iprocess_runner.h` + `process_runner_win.cpp` /
  `process_runner_linux.cpp`, `system_util_*`, `isystem_util.h`.
- **Rule.** One runner, serialized on purpose (see the comment in the header).

## 10. MCP client

- **Responsibility.** Talk to external MCP servers over stdio JSON-RPC and put
  their tools in front of the model.
- **Entry.** `src/core/mcp_manager.cpp` (`start` / `stop` / `syncTools` /
  `callTool`), `src/core/mcp_protocol.cpp` (newline framing, `_meta`,
  `resultType`), `src/core/mcp_process.cpp` (three pipes, Job Object / `killpg`),
  `src/core/mcp_config.cpp` (`projv_files/mcp.json`), `src/tui/mcp_view.cpp` (F4).
- **Rule.** One `worker_` thread per server, joined by `start` / `stop` /
  `shutdownAll`, never detached. `syncTools()` is the only writer of
  `ToolRegistry` and `Agent::autoToolNames_`, and it runs on the agent thread at a
  turn boundary. `mcp__<server>__<tool>` lives in `autoToolNames_`, never in the
  prompt allowlist -- an empty allowlist must keep meaning "allow everything".
- **Tests.** `test_mcp.cpp` plus the `fake_mcp_server` executable fixture in
  `tests/fakes/`, spawned as a real child process. `--mcp-check` / `--mcp-call`
  verify against a live server without starting the TUI.

---

Back -> [dev guide](README.md) | Recipes -> [recipes](recipes.md)
