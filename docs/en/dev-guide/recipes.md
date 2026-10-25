# Recipes

Each recipe: files to touch, in order -> what to change -> how to test -> how to
confirm by hand.

---

## 1. Add a built-in C++ tool

1. `src/tools/<name>_tool.cpp` + header: build a `ToolDefinition` (name,
   description, parameters) and register an executor with `registry.registerTool`.
2. `src/tui/app_tui.cpp` `setupTools()`: call your `register*Tool(...)`.
3. `CMakeLists.txt` `COMMON_SOURCES`: add the `.cpp`.
4. If skills should use it, add the name to a skill's `tools` array.
5. Test: `registry.execute("your_tool", args.dump())` in `tests/unit/`, using a
   temp directory as the workspace. No TUI needed.

Rules: validate every path with `validateToolPath`; return `Error: ...` strings,
never throw; keep the description honest about limits.

---

## 2. Add a pytool

1. `projv_files/pytool/<name>/tool.json` + `main.py` (`requirements.txt` if
   needed).
2. No code change: discovery is automatic (`python_tool_manager.cpp`).
3. Test: run the script by hand with JSON on stdin.
4. Confirm in the app: ask the agent to use it; the approval dialog must show it.

---

## 3. Add or change a skill

1. Edit `skills/<name>/config.json`: `agents`, `round_steps`, `on_converge`,
   `tools`, `max_rounds`, `max_parallel`, `thinking`, `stop_when`.
2. Edit the prompt `.md` files, one per agent.
3. Verb tools live in `skills/<name>/tools/*.json`.
4. Test: `tests/unit/test_skill_*.cpp` with scripted responders - set a
   responder for **every** agent, otherwise the test hits the real API.
5. Confirm: `/skill list`, then `/<name> <topic>`.

Remember: only agents listed in `round_steps` / `on_converge` may dispatch;
`tools` is shared by every agent in the skill.

---

## 4. Add a `/` command

1. `src/core/agent.cpp` `handleCommand()` (or `src/tui/app_tui.cpp` for
   UI-only commands) - add the branch before the skill trigger.
2. Update `QUICK_HELP` in `agent.cpp` so `/help` lists it.
3. Test: assert the return value and the message appended to the session.
4. Document it in `docs/*/user-guide/08-reference.md`.

---

## 5. Add a config key

1. `src/client/models.h` `AppConfig`: add the field.
2. `src/core/config.cpp`: read it in `load()`, default it in the "insert missing
   keys" block, write it in `save()`.
3. If the TUI edits it, `src/tui/config_view.cpp`.
4. Document it in `docs/*/user-guide/08-reference.md`.
5. Test: save a config with the key, reload, compare.

---

## 6. Add an API parameter

1. `src/client/models.h` `ChatRequest`: add the field with a default that keeps
   the current wire format.
2. `src/client/deepseek.cpp` `buildRequestBody`: emit it only when it differs
   from the API default (that is how `thinking` stays invisible when on).
3. Test: `client.buildRequestBody(req)` is public - assert on the JSON.
4. Docs: `08-reference.md` if a user can trigger it.

---

## 7. Add or change a UI panel

1. Component: `src/tui/<panel>_view.cpp`; wiring and layout in
   `src/tui/app_tui.cpp`.
2. Colours: `theme_colors.*` / `speaker_colors.*` - do not hardcode.
3. Golden test: `tests/golden/test_*_render.cpp` renders into `Screen::Create`,
   no terminal required.
4. Confirm by hand at 120x36 and at a narrow width (wrapping is the usual bug).

---

## 8. Change the skill engine

1. DSL types: `src/core/skill_config.h`; parsing and validation:
   `SkillRunner::loadFromJson` (`skill_runner.cpp`).
2. Execution: `SkillRunner::executeSteps` (actions, parallel, cancel),
   `mergeAction` (slots/votes).
3. One role's loop: `SubAgent::turn` (`sub_agent.cpp`) - tool dispatch, the
   nudge, concurrency of `request_agent` groups.
4. Tests: `test_skill_runner.cpp` for the interpreter, `test_skill_parallel.cpp`
   for dispatch, `test_skill_thinking.cpp` for the wire format.
5. Keep the invariants: UI callbacks only after a join; one turn per agent;
   every tool call gets a result.

---

Back -> [dev guide](README.md) | Modules -> [module map](module-map.md)
