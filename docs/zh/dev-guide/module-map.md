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
- **固定上下文注入点。** 目录树与 `AGENTS.md` 都由
  `TuiApp::injectWorkspaceContext()` 注入；启动、`F3`、`/clear` 三条路径共用它
  （`/clear` 走 `reinitConversation(true)`，保留会话库）。动态的 todo/overview
  消息由 `Agent::buildChatRequest()` 插在固定 system 块之后，前缀缓存才稳定。
- **一起看。** `chat_view.*`、`status_bar.*`、`status_line.*`、`approval_logic.*`、
  `config_view.*`、`skill_trigger.*`、`core/turn_requests.h`。
- **测试。** `tests/golden/*`、`test_approval_logic.cpp`、`test_skill_trigger.cpp`、
  `test_agents_md.cpp`、`test_turn_end.cpp`。

## 2. Markdown 渲染

- **职责。** 把流式文本变成带颜色、会折行的行。
- **入口。** `src/tui/markdown_text.cpp`（分词/折行）、`markdown_view.cpp`
  （FTXUI 元素）、`bubble_model.cpp`（气泡）。
- **diff 气泡。** `src/tui/diff_view.cpp` 把存下来的 unified diff 重新投影成两栏
  （左旧行号 | 右新行号，改动行左右同排）；聊天栏宽度不够 `kSplitMinWidth` 时退回
  单栏块。只有解析和渲染：库里只存 unified diff，所以老会话直接用新布局回放。
- **一起看。** `theme_colors.*`（颜色全部来自主题）；`chat_view.cpp` 负责把量到的
  聊天栏宽度传进来。
- **测试。** `test_markdown_text.cpp`、`test_bubble_model.cpp`、
  `test_diff_parse.cpp`、`golden/test_markdown_render.cpp`、
  `golden/test_chat_render.cpp`。

## 3. 主题

- **职责。** 读写主题 JSON、把语义角色映射到颜色、驱动发言人配色。
- **入口。** `theme_manager.cpp`、`theme_map.cpp`、`theme_colors.cpp`、
  `speaker_colors.cpp`，编辑器在 `theme_editor_view.cpp`。
- **测试。** `test_theme_*`、`test_speaker_colors.cpp`、`golden/test_theme_editor.cpp`。

## 4. Agent 核心

- **职责。** 回合循环：构造请求、流式接收、审批并执行工具、持久化、上下文紧张时压缩。
- **入口。** `src/core/agent.cpp`（`submitUserMessage`、工具阶段、回合结束）、
  `session.cpp`（上下文窗口 + 数据库）、`prompts_loader.cpp`（角色）、
  `context_assembler.{h,cpp}`（P1 环境块、P3 能力地图）、
  `project_context.cpp`（P2：目录树 + AGENTS.md 合成一条消息）。
- **消息布局。** 固定前缀 `[P0]` 角色、`[P1]` 环境、`[P2]` 项目 —— 三条落库的 system
  消息，而且永远是三条：P1/P2 空了也写占位，所以条数不会漂。`[P3]` 能力地图每轮由
  TUI 在 `McpManager::syncTools()` 之后重建，作为不透明文本交给 Agent，因此地图里列的
  就是这次请求真正带的工具。每轮动态块（用户请求、TODO、overview）插在前缀之后，
  `Session::applyCompaction()` 的摘要插在同一位置。
- **P3 为什么不落库。** MCP server 在轮边界才注册，skill 随时可能被写出来，存一份必然
  过期。同样的输入产生同样的字节（工具名、server 名、skill 名都排序），所以只要工具表
  不变，KV 前缀缓存就一直命中。
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
- **规矩。** 所有路径都过 `validateToolPath`（`file_tool.h`），它转调
  `ToolPaths::resolve`（`tool_paths.h`）。根有三类：workspace（可读可写）、
  `projv_files`（可读可写）、`projv_files/sessions` 与 `projv_files/logs`（只读）。
  命中**最深**的根说了算，所以两个只读子根能嵌在可写根里面。`forWrite` 是参数不是约定：
  只读根会拒掉 `write_file` / `edit_file` / `md_file` / `format_file`，用的是
  `Agent::run()` 匹配的 `__WORKSPACE_VIOLATION__:` 标记，并且错误文案会列出全部 root，
  这样模型重试才会换个合法路径。
- **接法。** `TuiApp::setupTools()` 用一次
  `ToolPaths::forSession(workspace, projv_files)` 建好，交给六个带路径的注册函数。
  测试里写 `ToolPaths(root)`（隐式单可写根）。
- **diff 捕获。** 四个写文件的工具在自己的写入前后各拍一次快照，交给
  `diff_capture::record`（`diff_capture.{h,cpp}`）。这个槽是 `thread_local` 而不是
  注册表成员——技能 worker 会并行共享同一个注册表。`Agent::executeTool` 在调用前
  `reset`、调用后 `take`，再挂到 tool 消息上。只用于显示：随消息行进库，永不序列化。
- **测试。** `test_file_tools.cpp`、`test_diff_capture.cpp`、`test_web_api.cpp`。

## 7. 技能引擎

- **职责。** 按 JSON 描述跑一个多 Agent 程序：固定角色、轮次、并行动作、槽位/投票、
  收敛、事件输出。
- **入口。** `src/core/skill_runner.cpp`（`run`、`executeSteps`、`mergeAction`、
  `handleRequestAgent`）、`sub_agent.cpp`（每个角色一个 LLM 循环 + 工具分发）、
  `skill_config.h`（DSL 与校验）。没有任何技能编在二进制里：技能就是用户（或 agent）
  写出来的一组文件。
- **并发。** 一条回复里可以有多个 `request_agent`：按目标分组，组间并发
  （受 `max_parallel` 限制），同一目标串行，结果按调用顺序回填。派活只有一层：
  只有 `round_steps` / `on_converge` 里的角色能拿到这个工具。
- **一起看。** `tools/registry.h`、`client/deepseek.h`、`skills/*/config.json`、
  `skills/_authoring/skill-authoring.md`（随包写作指南；没有 `config.json`，所以
  `listSkills()` 永远不会把它当成可运行技能）。
- **测试。** `test_skill_runner.cpp`、`test_skill_engine.cpp`、
  `test_skill_swarm.cpp`、`test_skill_parallel.cpp`、`test_skill_thinking.cpp`、
  `test_skill_monica.cpp`。

## 8. 存储与配置

- **职责。** `config.toml` 往返读写、SQLite 会话持久化。
- **入口。** `src/core/config.cpp`（TOML 读写与默认值）、`src/core/storage.cpp`
  （建库/开库、追加消息、checkpoint）。
- **表结构。** `messages` 有一列 `diff TEXT`：写文件的工具产出的 unified diff，
  在 TUI 里单独一个气泡显示。它只用于显示，绝不能进 `to_json`。老库靠与
  `reasoning_content` / `image_paths` / `synthetic` 同一段 try-`ALTER TABLE` 迁移。
- **测试。** `test_storage_diff.cpp` 覆盖 `diff` 的存取往返，以及从无 `diff` 列的
  旧库迁移；其余由技能与 agent 套件间接覆盖。

## 9. 平台抽象

- **职责。** 屏蔽 Windows 与 POSIX 的差异。
- **入口。** `src/platform/iprocess_runner.h` + `process_runner_win.cpp` /
  `process_runner_linux.cpp`、`system_util_*`、`isystem_util.h`。
- **规矩。** 一个 runner，串行是刻意的（见头文件注释）。

## 10. MCP 客户端

- **职责。** 用 stdio JSON-RPC 与外部 MCP server 对话，把它的工具交给模型。
- **入口。** `src/core/mcp_manager.cpp`（`start` / `stop` / `syncTools` /
  `callTool`）、`src/core/mcp_protocol.cpp`（换行分帧、`_meta`、`resultType`）、
  `src/core/mcp_process.cpp`（三根管道、Job Object / `killpg`）、
  `src/core/mcp_config.cpp`（`projv_files/mcp.json`）、`src/tui/mcp_view.cpp`（F4）。
- **规矩。** 每个 server 一个 `worker_` 线程，由 `start` / `stop` / `shutdownAll`
  负责 join，**永不 detach**。`syncTools()` 是 `ToolRegistry` 与
  `Agent::autoToolNames_` 的唯一写入方，只在回合边界的 agent 线程上跑。
  `mcp__<server>__<tool>` 只进 `autoToolNames_`，绝不进提示词白名单——空白名单必须
  继续表示"全放行"。
- **测试。** `test_mcp.cpp` 加上 `tests/fakes/` 里的 `fake_mcp_server` 可执行夹具
  （作为真正的子进程被拉起）。`--mcp-check` / `--mcp-call` 可以不开 TUI 直接对着
  真 server 验证。

---

返回 -> [开发指南](README.md) | 改动配方 -> [recipes](recipes.md)
