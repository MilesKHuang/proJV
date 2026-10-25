# 模块地图

> [English](../../en/dev-guide/module-map.md) | **中文**

每个模块一张路标卡片：一句话职责、入口、要一起看的文件、相关测试。文件名以代码为
准 —— 这页和代码不一致时，代码是对的。

---

## 1. 入口与 TUI 外壳

- **职责。** 独占终端，管应用状态机和事件循环。
- **入口。** `src/tui/main_tui.cpp` -> `src/tui/app_tui.cpp` 的 `setup()` 与
  `run()`。工具注册集中在 `setupTools()`。
- **数据流。** 按键 -> `TuiApp` -> agent 或技能；回调 -> 气泡 + 状态栏。
- **一起看。** `chat_view.*`、`status_bar.*`、`status_line.*`、`approval_logic.*`、
  `config_view.*`、`skill_trigger.*`。
- **测试。** `tests/golden/*`、`test_approval_logic.cpp`、`test_skill_trigger.cpp`。

## 2. Markdown 渲染

- **职责。** 把流式文本变成带颜色、会折行的行。
- **入口。** `src/tui/markdown_text.cpp`（分词/折行）、`markdown_view.cpp`
  （FTXUI 元素）、`bubble_model.cpp`（气泡）。
- **一起看。** `theme_colors.*`（颜色全部来自主题）。
- **测试。** `test_markdown_text.cpp`、`test_bubble_model.cpp`、
  `golden/test_markdown_render.cpp`。

## 3. 主题

- **职责。** 读写主题 JSON、把语义角色映射到颜色、驱动发言人配色。
- **入口。** `theme_manager.cpp`、`theme_map.cpp`、`theme_colors.cpp`、
  `speaker_colors.cpp`，编辑器在 `theme_editor_view.cpp`。
- **测试。** `test_theme_*`、`test_speaker_colors.cpp`、`golden/test_theme_editor.cpp`。

## 4. Agent 核心

- **职责。** 回合循环：构造请求、流式接收、审批并执行工具、持久化、上下文紧张时压缩。
- **入口。** `src/core/agent.cpp`（`submitUserMessage`、工具阶段、回合结束）、
  `session.cpp`（上下文窗口 + 数据库）、`prompts_loader.cpp`（角色）、
  `project_context.cpp`（注入工作区快照）。
- **数据流。** `AppConfig` + `Session` -> `ChatRequest` -> 回调 ->
  `Storage`（SQLite）+ UI。
- **一起看。** `client/models.h`、`tools/registry.h`、`config.*`。
- **测试。** `test_agent_request.cpp`、`test_turn_end.cpp`、
  `test_reasoning_content.cpp`、`test_prompt_*`。

## 5. API 客户端

- **职责。** 序列化请求、SSE 流式、重试、取消，以及官方 Search / Fetch 结果的格式化。
- **入口。** `src/client/deepseek.cpp`（`buildRequestBody`、`streamBlocking`、
  `parseSSEChunk`），类型在 `models.h` / `models.cpp`。
- **坑位。** 带 `tools` 时每条 assistant 消息都必须回传 `reasoning_content`；
  强制 `tool_choice` 与思考模式互斥（proJV 此时会关掉思考）。
- **测试。** `test_deepseek_sse.cpp`、`test_reasoning_content.cpp`、`test_image_input.cpp`。

## 6. 工具

- **职责。** 一个能力一个函数，通过注册表以 JSON schema 暴露。
- **入口。** `src/tools/registry.cpp`；`file_tool.cpp`（读/写/列目录/grep）、
  `edit_file_tool.cpp`、`md_file_tool.cpp`、`file_search_tool.cpp`、
  `format_tool.cpp`、`shell_tool.cpp`、`web_tools.cpp`、`todo_tool.cpp`、
  `python_tool_manager.cpp`。
- **规矩。** 所有路径都过 `validateToolPath`（`file_tool.h`）；工具绝不碰工作区外的文件。
- **测试。** `test_file_tools.cpp`、`test_web_api.cpp`。

## 7. 技能引擎

- **职责。** 按 JSON 描述跑一个多 Agent 程序：固定角色、轮次、并行动作、槽位/投票、
  收敛、事件输出。
- **入口。** `src/core/skill_runner.cpp`（`run`、`executeSteps`、`mergeAction`、
  `handleRequestAgent`）、`sub_agent.cpp`（每个角色一个 LLM 循环 + 工具分发）、
  `skill_config.h`（DSL 与校验）、`skills_builtin.cpp`（自播种的 `skill_maker`）。
- **并发。** 一条回复里可以有多个 `request_agent`：按目标分组，组间并发
  （受 `max_parallel` 限制），同一目标串行，结果按调用顺序回填。派活只有一层：
  只有 `round_steps` / `on_converge` 里的角色能拿到这个工具。
- **一起看。** `tools/registry.h`、`client/deepseek.h`、`skills/*/config.json`。
- **测试。** `test_skill_runner.cpp`、`test_skill_engine.cpp`、
  `test_skill_swarm.cpp`、`test_skill_parallel.cpp`、`test_skill_thinking.cpp`、
  `test_skill_monica.cpp`、`test_skill_maker.cpp`。

## 8. 存储与配置

- **职责。** `config.toml` 往返读写、SQLite 会话持久化。
- **入口。** `src/core/config.cpp`（TOML 读写与默认值）、`src/core/storage.cpp`
  （建库/开库、追加消息、checkpoint）。
- **测试。** 由各 agent / 技能测试间接覆盖，无独立套件。

## 9. 平台抽象

- **职责。** 屏蔽 Windows 与 POSIX 的差异。
- **入口。** `src/platform/iprocess_runner.h` + `process_runner_win.cpp` /
  `process_runner_linux.cpp`、`system_util_*`、`isystem_util.h`。
- **规矩。** 一个 runner，串行是刻意的（见头文件注释）。

---

返回 -> [开发指南](README.md) | 改动配方 -> [recipes](recipes.md)
