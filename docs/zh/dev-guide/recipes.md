# 改动配方

> [English](../../en/dev-guide/recipes.md) | **中文**

每个配方：按顺序列出要改的文件 -> 改什么 -> 怎么测 -> 怎么手动确认。

---

## 1. 新增内置 C++ 工具

1. `src/tools/<名字>_tool.cpp` + 头文件：构造 `ToolDefinition`（名字、描述、参数），
   用 `registry.registerTool` 注册执行体。
2. `src/tui/app_tui.cpp` 的 `setupTools()`：调用你的 `register*Tool(...)`。
3. `CMakeLists.txt` 的 `COMMON_SOURCES`：加入这个 `.cpp`。
4. 想让技能能用，把它加进某个技能的 `tools` 数组。
5. 测试：在 `tests/unit/` 里用临时目录当工作区，直接 `registry.execute(...)`，
   不需要 TUI。

规矩：所有路径过 `validateToolPath`；返回 `Error: ...` 字符串，不要抛异常；
描述要如实写清限制。

---

## 2. 新增 pytool

1. `projv_files/pytool/<名字>/tool.json` + `main.py`（有依赖再加 `requirements.txt`）。
2. 无需改代码：发现是自动的（`python_tool_manager.cpp`）。
3. 测试：手工喂 JSON 到 stdin 跑一遍脚本。
4. 应用里确认：让 agent 用它，审批弹窗里应能看到。

---

## 3. 新增或修改技能

1. 改 `skills/<名字>/config.json`：`agents`、`round_steps`、`on_converge`、`tools`、
   `max_rounds`、`max_parallel`、`thinking`、`stop_when`。
2. 改提示词 `.md`，一个角色一份。
3. 动作工具放 `skills/<名字>/tools/*.json`。
4. 测试：`tests/unit/test_skill_*.cpp` 用脚本化 responder —— **每个** agent 都要设，
   漏一个就会走真实 API。
5. 确认：`/skill list`，然后 `/<名字> <主题>`。

注意：只有 `round_steps` / `on_converge` 里出现的角色能派活；`tools` 是技能里所有
角色共享的。

---

## 4. 新增 `/` 命令

1. `src/core/agent.cpp` 的 `handleCommand()`（纯界面的命令放
   `src/tui/app_tui.cpp`）—— 分支要加在技能触发之前。
2. 同步更新 `agent.cpp` 里的 `QUICK_HELP`，否则 `/help` 里看不到。
3. 测试：断言返回值以及写入会话的消息。
4. 文档：更新 `docs/*/user-guide/08-reference.md`。

---

## 5. 新增配置项

1. `src/client/models.h` 的 `AppConfig`：加字段。
2. `src/core/config.cpp`：`load()` 里读、补默认值的分支里补、`save()` 里写。
3. 需要界面可改就动 `src/tui/config_view.cpp`。
4. 文档：`docs/*/user-guide/08-reference.md`。
5. 测试：写一份带该键的配置，重新加载并比对。

---

## 6. 新增 API 参数

1. `src/client/models.h` 的 `ChatRequest`：加字段，默认值要保证「不发这个字段」
   等于现在的线上行为。
2. `src/client/deepseek.cpp` 的 `buildRequestBody`：只在偏离 API 默认值时才发
   （`thinking` 就是这么做的）。
3. 测试：`buildRequestBody` 是公开的，直接断言 JSON。
4. 文档：用户能触发的话写进 `08-reference.md`。

---

## 7. 新增或修改界面面板

1. 组件：`src/tui/<面板>_view.cpp`；布局与接线在 `src/tui/app_tui.cpp`。
2. 配色走 `theme_colors.*` / `speaker_colors.*`，不要写死。
3. golden 测试：`tests/golden/test_*_render.cpp` 用 `Screen::Create` 在内存渲染。
4. 手动确认：120x36 和窄宽度各试一次（折行是最常见的 bug）。

---

## 8. 修改技能引擎

1. DSL 类型：`src/core/skill_config.h`；解析与校验：
   `SkillRunner::loadFromJson`（`skill_runner.cpp`）。
2. 执行：`SkillRunner::executeSteps`（动作、并行、取消）、`mergeAction`（槽位/投票）。
3. 单个角色的循环：`SubAgent::turn`（`sub_agent.cpp`）—— 工具分发、漏调补救、
   `request_agent` 分组并发。
4. 测试：`test_skill_runner.cpp` 测解释器，`test_skill_parallel.cpp` 测派活，
   `test_skill_thinking.cpp` 测请求体。
5. 守住不变量：UI 回调只在 join 之后；一个 agent 同时只有一个 turn；
   每个工具调用都必须有结果。

---

返回 -> [开发指南](README.md) | 模块 -> [模块地图](module-map.md)
