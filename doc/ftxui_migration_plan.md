# proJV 前端迁移（ImGui → FTXUI）与流式改造 —— 多阶段执行计划

- 日期：2026-09-11
- 分支：`dev-049`（与 `origin/dev-049` 同步）
- 范围：需求 R2（上下文残留修复）+ 需求 R1（流式显示）+ 需求 R3（前端换 FTXUI）
- 状态：✅ 已完成（2026-09-13，见 `RELEASE_NOTES_v0.5.0.md` 与 `tui_parity_gap.md`）

---

## 0. 目标与总体策略

三个需求合并为一个执行计划：

| 编号 | 需求 | 处理方式 |
|------|------|----------|
| R2 | new session 后 overview/TODO 上下文残留 | **Phase 0 独立先行**，修复后推送远端 |
| R1 | reasoning 与回复流式显示 | 并入迁移，在 Phase 5 落地 |
| R3 | 前端整体从 Dear ImGui 换成 external/FTXUI-7.0.3 | 迁移主体，Phase 1–10 |

核心原则：

1. **后端冻结**：`src/client/`、`src/core/`、`src/tools/`、`src/platform/` 不依赖 ImGui，迁移期间保持行为不变，只做必要的可测试性改造（见 §3）。
2. **1:1 复刻**：前端以"功能"为单位细粒度移植，每个功能点都对照附录 A 的行为清单逐条验收，不允许静默改变行为。
3. **每阶段独立可编译、可运行、可验收**：任何阶段结束后 `proJV` 都必须能构建并通过该阶段的测试门禁，不追求"一步到位"。
4. **行为逻辑下沉为纯函数 + 测试**：把"Message → 气泡""工具消息格式化""Markdown → 文本"等逻辑从 UI 渲染中剥离成无 UI 依赖的纯函数，UI 层只做薄壳。这是"1:1 无疏漏"的机制保障（见 §3.4）。

### 0.1 关键事实（已核实）

- 后端链路已经是流式的：`DeepSeekClient::streamBlocking` 在 agent 线程同步收 SSE，`parseSSEChunk` 逐 delta 回调 `onText`/`onThinking`，分别追加到 `currentContent_`/`currentReasoning_`。
- 前端**当前不是真流式**：`App::syncChatFromAgent()` 注释明确 "No live streaming bubble"，聊天气泡只从 SQLite 持久化消息派生；实时反馈只有输入区上方的字符计数（`Reasoning... (N chars)`）。
- `external/FTXUI-7.0.3/` 已就位（CMake 目标：`ftxui::screen` / `ftxui::dom` / `ftxui::component`，另有 `ftxui::ftxui` 聚合目标）。
- `external/FTXUI-7.0.3/` 目前是 **git 未跟踪目录**，Phase 0 提交时不能误提交（见 §4.6）。

---

## 1. 现状架构

```
src/main.cpp          入口 + ImGui 主循环（GLFW 事件循环）
src/gui_backend.cpp   GLFW + OpenGL3 + ImGui 窗口/渲染后端
src/ui/app.cpp        App 类：渲染编排 + 会话/线程管理（106 处 ImGui::）
src/ui/render_chat.cpp    聊天气泡渲染（143 处 ImGui::）
src/ui/render_settings.cpp 配置弹窗 + 工具审批弹窗（79 处 ImGui::）
src/ui/markdown_render.cpp ImGui 版 Markdown 渲染（89 处 ImGui::）
src/ui/theme.cpp           主题颜色模型 + ThemeManager（2 处 ImGui::）
src/ui/theme_popup.cpp     主题编辑器弹窗（116 处 ImGui::）
─────────────────────────────────────────── 前端/后端边界
src/client/   models / deepseek（SSE、libcurl、JSON）          ← 无 ImGui
src/core/     config / agent / session / storage / prompts     ← 无 ImGui
src/tools/    registry / file / md_file / edit_file / search   ← 无 ImGui
              web / todo / shell / python_tool_manager
src/platform/ isystem_util / iprocess_runner / system_util     ← 无 ImGui
```

### 1.1 前端依赖的后端接口（迁移期间必须保持稳定）

新前端（FTXUI）复用同一套接口，不得绕过：

- `Agent`：`getStatus()` → `AgentStatus`、`getPhase()` → `AgentPhase`、`getNewMessagesSince(id)` → `vector<Message>`、`copyTodoData()` → `TodoData`、`startTurn(text)`、`approveTool(action)`、`cancel()`、`setSystemPrompt()` / `replaceSystemPrompt()`、`setModel()` / `setToolPaths()` / `setRequestParams()`、`getContextPressure()` / `getEstimatedContextTokens()` / `getContextWindowSize()`、`compactSession()`、`clearSession()`、`newTurn()`。
- `AgentStatus`（`src/client/models.h`）：`state`、`streamingText`、`reasoningText`、`errorMessage`、`statusMessage`、`toolProgressCurrent/Total`、`currentToolName`。
- `AgentPhase`：`Idle / Streaming / ExecutingTools / AwaitApproval / Error`。
- `AppConfig`：`load()`/`save()`，字段见 `models.h`。
- `Storage`：`createDatabase()`/`openDatabase()`/`closeDatabase()`/`insertMessage()`/`queryMessages()`。
- `ThemeManager`/`ThemeColors`：`init/switchTo/applyCustom/export/load`、`onThemeChanged`。
- `SystemUtil`：`OpenUrl`、字体/文件对话框/剪贴板辅助。
- `DeepSeekClient`：`fetchModels()`（模型列表）。

### 1.2 会话/消息数据流（保持不变）

```
输入 → agent->startTurn(text) → launchAgentThread → agent->run()（后台线程）
     → SSE 流式回调 → currentContent_/currentReasoning_ 累积
     → status_.streamingText / reasoningText 快照（BATCH_INTERVAL=16）
     → addPersistedMessage → session.addMessage + storage.insertMessage（异步）
前端每帧：getStatus() 读快照 + getNewMessagesSince(lastMessageId_) 读 DB 增量 → 派生气泡渲染
```

---

## 2. 执行顺序总览

```
Phase 0   R2 修复 ──────────────────────────► 提交 + push origin/dev-049
Phase 1   FTXUI 骨架 + 终端初始化 + 最小闭环
Phase 2   数据模型 + 气泡派生（纯函数下沉）      ◄── 可测试性改造
Phase 3   聊天渲染（非流式气泡，1:1）
Phase 4   Markdown 渲染
Phase 5   流式显示（R1）                      ◄── 需求 1 落地
Phase 6   输入区 + 状态行 + 状态栏
Phase 7   TODO 面板
Phase 8   配置弹窗 + 审批弹窗 + 欢迎页
Phase 9   主题系统 + 主题编辑器
Phase 10  菜单/会话切换/文件对话框 + 清理 ImGui + 双端回归
```

> **测试分工约定**：自动化测试（单元 + golden）在对应阶段内跑并作为该阶段门禁；所有需要人工肉眼比对、真实 API、双平台、或依赖真实终端环境的验证项，**统一收集到附录 C「手工待测项清单」**。全部阶段完成后按清单逐项勾选验收，阶段正文不再散落"手工 A/B"，仅标注"见附录 C.x"。

---

## 3. 测试基础设施规划（先行，所有阶段依赖）

### 3.1 测试框架

- 单元测试：**doctest**（单头文件，vendored 到 `external/doctest/doctest.h`）。理由：零构建、与项目"零包管理依赖"哲学一致、语法近似 Catch2。
  - 备选：Catch2 v3（header-only，需 CMake 集成）或 GoogleTest。
- 测试 runner：CTest（CMake 内置，无需新依赖）。
- 前端渲染测试：FTXUI 的 `ftxui::Screen` 渲染到内存 buffer，`screen.ToString()` 拿文本做 **golden test**（这是 FTXUI 相对 ImGui 的测试优势——ImGui 很难做自动化 UI 断言）。

### 3.2 目录结构

```
proJV/
├── tests/
│   ├── CMakeLists.txt
│   ├── unit/                        # 后端/纯逻辑单元测试
│   │   ├── test_todo.cpp            # update_todo 生命周期 + overview prune
│   │   ├── test_session.cpp         # compress / prune / serialize
│   │   ├── test_storage.cpp         # SQLite CRUD（临时库）
│   │   ├── test_bubble_model.cpp    # Message→气泡派生（1:1 规则）
│   │   ├── test_markdown_text.cpp   # markdown→终端文本
│   │   ├── test_theme.cpp           # ThemeColors JSON 序列化 + 颜色映射
│   │   ├── test_agent_request.cpp   # buildChatRequest 注入/清空（含 R2 回归）
│   │   └── test_deepseek_sse.cpp    # parseSSEChunk 流式解析
│   └── golden/                      # FTXUI 渲染 golden test
│       ├── test_chat_render.cpp
│       ├── test_todo_render.cpp
│       └── ...
└── src/
    ├── tui/                         # 新前端（FTXUI）
    └── ui/                          # 旧前端（Phase 10 删除）
```

### 3.3 CMake 集成（骨架）

```cmake
# tests/CMakeLists.txt（节选）
find_package(doctest CONFIG QUIET)      # 若无则用 vendored 头
add_subdirectory(${EXTERNAL_DIR}/FTXUI-7.0.3 ${CMAKE_BINARY_DIR}/ftxui)  # 或顶层引入

add_executable(proJV_tests
    unit/test_todo.cpp unit/test_bubble_model.cpp ...)
target_link_libraries(proJV_tests PRIVATE proJV_lib doctest)
include(CTest)
add_test(NAME proJV_tests COMMAND proJV_tests)
```

> 注意：FTXUI 的 `add_subdirectory` 建议只引入 `screen`/`dom`/`component` 目标；测试关闭 `FTXUI_BUILD_TESTS/EXAMPLES/DOCS`。

### 3.4 可测试性改造（关键机制）

当前这些逻辑埋在 ImGui 渲染代码里，无法直接测试。迁移时把它们抽成 **无 UI 依赖的纯函数模块**（`src/tui/` 下），UI 层只负责调用：

| 下沉模块 | 来源 | 职责 | 测试方式 |
|----------|------|------|----------|
| `bubble_model` | `deriveBubblesFromMessage` + `formatToolMsg` | Message → `vector<Bubble>` 的完整派生规则 | 单元测试，逐规则断言 |
| `markdown_text` | `markdown_render.cpp` | Markdown 文本 → 终端文本/装饰序列 | 单元测试，用例集对照 |
| `theme_map` | `theme.cpp` 的 `toVec4` | 主题 hex → FTXUI Color/装饰 | 单元测试，全字段断言 |
| `status_line` | `render_chat.cpp` 状态行 | `AgentStatus` → 状态文本 | 单元测试，逐状态断言 |
| `bubble_render` | `renderChatArea` | `vector<Bubble>` → FTXUI Element | golden test（Screen 捕获） |

这套下沉改造是"1:1 无疏漏"的保证：**行为在纯函数里被测试锁死，换渲染壳不影响行为**。

---

## 4. Phase 0：R2 修复（先行，独立交付）

### 4.1 根因

- `TodoData`（`pendingTodo`、`pendingBriefs`、`overviewSummary`、`activeUserPrompt`）是 `Agent` 成员，受 `todoMutex` 保护。
- `Agent::buildChatRequest()` 每次通过 `buildTodoSystemMessage()` / `buildOverviewSystemMessage()` / `buildUserRequestSystemMessage()` 把三者注入请求（插在 system prompt 之后）。
- `newChat()` 只做了 `agent->setStorage(nullptr)` + `agent->clearSession()`；而 `clearSession()` 里只 `session.clear()` 和重置状态，**从未清空 `todoData`**。
- 因此 new session 后 `overviewSummary`（README 标为"永久保留"）与 `pendingTodo` 继续存活并被注入新会话上下文。`switchToDialog()`（Open Chat）同根因。

### 4.2 修复方案（采用"彻底清空"语义）

在 `Agent::clearSession()` 中清空 `todoData`：

```cpp
void Agent::clearSession() {
    cancel(); session.clear();
    contextTokensDirty_= /* 原值 */;
    toolCallDepth_ = 0; destructiveApproved_.store(false);
    phase_ = AgentPhase::Idle;
    {
        std::lock_guard<std::mutex> lk(todoMutex);
        todoData = TodoData{};      // ★ 新增：new session 后清空 overview/TODO/请求上下文
    }
    { /* 原 status 重置逻辑不变 */ }
}
```

说明：同一会话内（同一 new chat 的多轮对话）`todoData` 仍正常归档/注入，不受影响；只有 `clearSession()`（new chat / switch dialog / `/clear`）才清空。`/clear` 走 `handleQuickCommand` → `clearSession()`，因此行为一致。

> 语义备注：README 的"概览自动归档、永久保留"指的是同一会话内跨轮保留；跨 `new session` 保留被判定为 bug。若未来要"归档展示仍保留但断开注入"，需把 overview 归档从 `todoData` 拆到独立存储——本期不做，记为决策点 D1（见 §9）。

### 4.3 改动文件

- `src/core/agent.cpp`：`clearSession()` 加一行清空逻辑（见 4.2）。

### 4.4 测试用例（Phase 0 验收）

新增 `tests/unit/test_agent_request.cpp`（或临时测试），至少覆盖：

1. **清空行为**：通过 `ToolRegistry` 执行 `update_todo`（`init` → `update` → `done`）写入 `todoData`，调用 `agent.clearSession()`，断言 `copyTodoData()` 四个字段均为空。
2. **注入行为（回归）**：`todoData` 非空时 `buildChatRequest()` 注入 `[TODO]`、`[OVERVIEW]`、`[USER REQUEST ON-GOING]` 前缀的 system 消息；清空后这些前缀不再出现（仅剩 `[TOOL PATHS]`）。
3. **边界**：`clearSession` 后再次 `startTurn` 不影响后续正常 `update_todo init`（不破坏工具生命周期）。

> 若 `todoData` 为 private 无 setter，测试通过 `ToolRegistry` 执行 `update_todo` 工具间接写入（`registerTodoTool` 已注册），避免为测试暴露成员。

### 4.5 提交与推送

```bash
cd D:\ws\proJV
git add src/core/agent.cpp tests/unit/test_agent_request.cpp tests/CMakeLists.txt
git commit -m "fix: clear todo/overview context on new session (R2)"
git push origin dev-049
```

### 4.6 提交注意事项（重要）

- `external/FTXUI-7.0.3/` 是未跟踪目录，**禁止 `git add -A`**，否则会把整个 FTXUI 源码带进 R2 提交。用精确路径 `git add`。
- 是否把 FTXUI 纳入版本库、以及是否需要 `.gitignore` 处理，在 Phase 1 决策（见 §9 D2）。

---

## 5. Phase 1：FTXUI 骨架 + 终端初始化 + 最小闭环

### 5.1 目标

让项目在**保留后端不动**的前提下，能用一个 FTXUI 终端程序启动、显示静态界面、接收输入、退出。

### 5.2 移植内容

- CMake：引入 `external/FTXUI-7.0.3`（`add_subdirectory`，关闭 examples/tests/docs）；新增 `proJV_tui` 可执行目标，链接 `proJV_lib` + `ftxui::screen`/`dom`/`component`。
- 新增 `src/tui/main_tui.cpp`：`ftxui::ScreenInteractive` 主循环，替换 GLFW 事件循环。
- 新增 `src/tui/terminal.cpp/h`：Windows 控制台初始化（`SetConsoleOutputCP(CP_UTF8)`、`SetConsoleCP(CP_UTF8)`、`ENABLE_VIRTUAL_TERMINAL_PROCESSING`）+ UTF-8 输入；Linux 免初始化。
- 最小闭环：一个输入框 + 一个输出区，回车 echo 文本，Esc/`q` 退出。

### 5.3 测试 / 验收

- Windows（MSVC）与 Linux 均编译通过。
- 终端启动：显示正常、可输入、可退出。
- **中文显示**：输入/输出一段中文（含全角标点），确认宽度对齐正确（FTXUI 的 CJK 宽度是主要风险，Phase 1 就要暴露，不要拖到最后）。
- 验收门禁：新增 `tests/golden/test_tui_smoke.cpp`，用 `Screen` 渲染最小界面断言非空输出。

### 5.4 决策点

- 旧 `proJV`（ImGui）与 `proJV_tui` 在迁移期间**并行存在**，便于随时用旧版对照行为；Phase 10 才删除旧版。这样每个阶段都能 A/B 对照验证。

---

## 6. Phase 2：数据模型 + 气泡派生（纯函数下沉）

### 6.1 目标

把"Message → 气泡"的派生规则和"工具消息格式化"从 `render_chat.cpp` 抽成纯函数，并补齐单元测试锁死 1:1 行为。

### 6.2 移植内容

新建 `src/tui/bubble_model.h/cpp`，从 `App::deriveBubblesFromMessage()` 与 `formatToolMsg()` 平移，**不改动任何规则**：

- `formatToolMsg(Message) → {role, content}`（工具调用/结果格式化）
- `deriveBubbles(Message) → vector<Bubble>`（含 reasoning 前置气泡、正文气泡、tool_call 气泡的完整派生顺序）

`Bubble` 结构对齐现有 `ChatBubble`（`role/content/reasoningText/hasReasoning/reasoningExpanded`）。

### 6.3 测试用例（附录 A 逐条覆盖）

- system 消息：`[Context compacted:` 显示 / `[Workspace]` 显示 / 其他跳过。
- assistant+tool_calls：reasoning 前置、正文前置、tool_call 气泡顺序。
- role=="tool"：`read_file`/`file_search`/`grep_files` → `name (N lines, N bytes)`；`exec_shell`/`shell`/`git_*`/`web_search` 及其他 → `name (N bytes)`；首行 `-> first line`（80 截断）。
- tool_call 关键参数提取：`read_file/write_file→path(file)`、`exec_shell/shell→command`、`edit_file→path`、`grep_files→pattern`、`file_search→query`；解析失败→原始 args 截断 60。

### 6.4 验收

`proJV_tests` 全绿；与旧版对同一组 Message 手工对照输出一致。

---

## 7. Phase 3：聊天渲染（非流式气泡，1:1）

### 7.1 目标

用 FTXUI 复刻 `renderChatArea()` 的气泡渲染，非流式（此时仍从 DB 派生）。

### 7.2 移植内容

- `src/tui/chat_view.cpp`：`vector<Bubble>` → FTXUI `Element`，复刻：
  - tool_call 气泡：合并后续 tool_result（`----` 分隔）、`\n[tool:` 分隔 aiText/toolTitle、正文 85% 宽、tool_result >5 行截断加 `...`。
  - reasoning 折叠卡片：`[+]/[-] Reasoning (N chars)` 可切换、展开 body（高度上限 6 行、滚动）。
  - 普通气泡：user 右对齐 85% 宽；`[Context compacted:` 黄色样式 + 顶部标签；assistant 走 Markdown、其他纯文本。
  - 可见窗口：最后 100 气泡 + 顶部 `[ Showing last N of M messages ]`；chatHistory 上限 200。
  - 自动滚动。
- `syncChatFromAgent` / `buildBubblesFromMessages` 逻辑平移（DB 驱动增量 + `lastMessageId_` + 200 上限）。
- 复制按钮：FTXUI 无剪贴板直接写入，改为按键/菜单触发复制（用 `SystemUtil` 写剪贴板），记为行为差异 D3。

### 7.3 测试 / 验收

- `tests/golden/test_chat_render.cpp`：给定固定 `vector<Bubble>`，断言 Screen 文本包含关键标记（气泡文本、`Reasoning (N chars)`、`[Context compacted]`、`[ Showing last ...]` 等）。
- 手工 A/B：加载同一会话 DB，逐屏对照旧版输出。

---

## 8. Phase 4：Markdown 渲染

### 8.1 目标

把 `markdown_render.cpp` 的 ImGui 渲染（表格/代码块/粗斜体/代码/链接/标题/列表/引用/水平线）替换为"Markdown → 终端文本"纯函数，装饰用颜色/字符表现。

### 8.2 移植内容

- 新建 `src/tui/markdown_text.h/cpp`：纯函数 `renderMarkdownText(text) → 结构化文本段（含样式标记）`，样式标记由 `theme_map` 映射为 FTXUI 装饰。
- 规则对齐现有 `markdown_render.cpp`：
  - 表格 → 对齐的文本行（终端宽度受限时折行/截断，需明确策略）
  - 代码块 → 单色背景块 + 边框
  - 粗体/斜体/行内代码 → 颜色区分（保持文本原样，与现行为一致）
  - 链接 → 可点击（FTXUI 需自定义鼠标/焦点处理，回调 `SystemUtil::OpenUrl`）
  - 标题/列表/引用/水平线 → 对应文本样式

### 8.3 测试 / 验收

- `tests/unit/test_markdown_text.cpp`：准备 Markdown 用例集（标题、粗斜体、代码块、表格、链接、列表、引用、水平线、嵌套），逐条断言输出。
- 已知风险：**终端没有真正表格/图片**，表格在窄终端下排版退化为文本行——需在 §9 明确接受标准（D4）。

---

## 9. Phase 5：流式显示（R1）

### 9.1 目标

实现 reasoning 与回复的**逐字/逐块实时显示**，结束无重复、无闪烁、正确落为正式气泡。

### 9.2 后端微调（可选，量小）

- `Agent::makeCallbacks`：`BATCH_INTERVAL` 从 16 降到 1（或去掉批处理，改为每 token 刷 `status_.streamingText/reasoningText`）。现有批处理为减锁竞争而设，但 UI 每帧读快照，token 到达速率远低于帧率，去掉风险低。
  - 若担心锁竞争，改为"时间节流"（如每 8ms 刷一次）替代"计数节流"。

### 9.3 前端改动

- `src/tui/chat_view.cpp`：新增"临时流式气泡"渲染路径——当 `phase == Streaming` 且 `status.streamingText`/`reasoningText` 非空时，直接在气泡列表末尾渲染实时内容（reasoning 用折叠卡片、content 用正文）。
- 结束切换去重：`onFinish` 后消息经 `addPersistedMessage` 落库，`syncChatFromAgent` 派生出正式气泡；临时气泡按"同一 reasoning/content 前缀"或 message id 去重，保证无重复。
- 状态行（Phase 6）同时保留字符计数作为辅助信息。

### 9.4 测试 / 验收

- `tests/unit/test_agent_request.cpp` 或新 `test_streaming.cpp`：mock `StreamCallbacks`，验证 `onText`/`onThinking` 逐段累积到 `streamingText`/`reasoningText`、`onFinish` 全量 flush、快照刷新频率符合配置。
- `test_deepseek_sse.cpp`：喂真实 SSE 文本（含 `reasoning_content`、`content`、`tool_calls`、`finish_reason`、`usage`），断言回调序列与累积结果。
- 手工验收：真实 API 流式逐字出现；reasoning 与 content 分阶段正确；结束后无重复气泡、无闪烁。

---

## 10. Phase 6：输入区 + 状态行 + 状态栏

### 10.1 移植内容

- `src/tui/input_area.cpp`：prompt 选择器（隐藏 `compactor.md`）、输入框、发送按钮（复用 `agent->startTurn` + `launchAgentThread` 的等价逻辑）。
- 状态行（`status_line` 纯函数）：逐状态复刻 §附录 A-D：
  - `Thinking`：`Reasoning... (N chars)`（仅有 reasoning 无 content）或 `Generating... (N chars)` + `| reasoned N chars`。
  - `ExecutingTool`：`Running: name (i/n)`。
  - `AwaitingApproval`：`Awaiting approval...`。
  - 其他/Idle：`statusMessage` / `Idle`。
- 状态栏（`status_bar`）：模型名、token（输入+输出）、消息数、工具数、上下文压力%（对齐 `renderStatusBar`）。

### 10.2 测试 / 验收

- `tests/unit/test_status_line.cpp`：每个 `AgentState` 组合断言文本与计数格式。
- golden：状态栏渲染文本断言。
- 手工 A/B 对照。

---

## 11. Phase 7：TODO 面板

### 11.1 移植内容

- `src/tui/todo_view.cpp`：复刻 `renderTodoPanel()`——标题 `TODO` + `pendingTodo` 逐行渲染，颜色规则：`[x]`→done、`[*]`→in_progress、`[ ]`→open、默认 pending、空→`(No active tasks)`。
- **行为事实**：当前面板只渲染 `pendingTodo`，不渲染 `overviewSummary`（overview 仅注入上下文）。迁移保持原样，不新增展示；若想展示 overview，记为 D5。

### 11.2 测试 / 验收

- `tests/golden/test_todo_render.cpp`：给定 TodoData 断言各状态行颜色/文本；空态断言 `(No active tasks)`。

---

## 12. Phase 8：配置弹窗 + 审批弹窗 + 欢迎页

### 12.1 移植内容

- 配置（`config_view.cpp`）：apiKey、baseUrl、maxTokens（512–65536）、temperature（0–2）、workspace、cpp compiler、python；`Save & Connect` 写 `AppConfig` + `setToolPaths` + `setRequestParams` + 重新注册工具；`Cancel`。
- 审批（`approval_view.cpp`）：`YES -- Execute Once` / `ALWAYS -- Allow This Session` / `NO -- Cancel`，显示破坏性命令列表（对齐 `renderToolApprovalDialog`）。
- 欢迎页（`welcome_view.cpp`）：无 API key 时显示（对齐 `renderWelcomePage`）。

### 12.2 测试 / 验收

- 单元测试：配置保存→`AppConfig` 字段、`setRequestParams` 参数传递（可 mock）。
- golden：审批弹窗三按钮文本、欢迎页文本。
- 手工 A/B。

---

## 13. Phase 9：主题系统 + 主题编辑器

### 13.1 移植内容

- `src/tui/theme_map.cpp`：`ThemeColors`（60+ 字段）hex → FTXUI `Color`/装饰；`applyToImGui()` 对应改为 `applyToFtui()`。
- `ThemeManager` 平移：`init/switchTo/applyCustom/exportToFile/loadFromFile/scanThemeDir`、`onThemeChanged`（存 `config.toml`）。
- 主题编辑器（`theme_editor_view.cpp`）：逐分类逐色调整、实时预览、JSON 导入/导出（对齐 `theme_popup.cpp`，116 处 ImGui 调用，工作量最大）。

### 13.2 测试 / 验收

- `tests/unit/test_theme.cpp`：`ThemeColors::toJson`/`fromJson` 全字段 round-trip；hex→Color 全字段断言。
- 手工：切换 8 套主题、逐色调整、JSON 导入导出与旧版一致。

---

## 14. Phase 10：菜单/会话切换/文件对话框 + 清理 + 双端回归

### 14.1 移植内容

- 菜单：File（New Chat `Ctrl+N` / Save As `Ctrl+S` / Open Chat `Ctrl+O` / Exit）+ Settings（Configuration / Theme 选择 / Customize）。
- 会话切换：`newChat` / `switchToDialog` / `saveDialogToFile` / `loadDialogFromFile`（含 `ensureStorage`、`buildBubblesFromMessages`、`lastMessageId_` 重置）。
- 文件对话框：Windows `pickOpenPath`/`SetWindowTextA` → 终端输入路径交互（记为行为差异 D6）。
- 清理：删除 `src/ui/`、`src/gui_backend.*`、旧 `main.cpp` 的 ImGui/GLFW 依赖；CMake 移除 ImGui/GLFW/OpenGL 目标；`add_executable` 指向 `tui/main_tui.cpp`。
- **双端回归**：用旧版与新版对同一会话 DB 逐功能对照，确认无行为漂移后才删除旧版。

### 14.2 测试 / 验收

- 全量 `proJV_tests` + golden 通过。
- 手工回归清单（附录 A 全部条目）。
- Windows + Linux 双平台构建。

---

## 15. 附录 A：1:1 行为对照清单（验收基线）

### A. 消息 → 气泡派生（`deriveBubblesFromMessage`）

1. `role=="system"`：含 `[Context compacted:` → 显示；含 `[Workspace]` → 显示；其他 → 跳过。
2. 有 `toolCalls` 的 assistant 或 `role=="tool"`：
   - assistant 且 `reasoningContent` 非空 → 先插 reasoning 气泡（`content` 空）。
   - assistant 有 `toolCalls` 且有 `content` → 先插正文气泡，再插 tool_call 气泡。
   - tool_call 格式：每工具一行 `name: keyarg`（参数规则见下）；解析失败 → `name: rawargs(截断60)`。
   - tool_result 格式：`read_file/file_search/grep_files` → `name (N lines, N bytes)`；`exec_shell/shell/git_log/git_status/git_diff/web_search` 及其他 → `name (N bytes)`；追加 `  -> firstline(截断80)`。
3. 普通 user/assistant：assistant 且有 `reasoningContent` → 先插 reasoning 气泡；再插正文气泡。

### B. 气泡渲染（`renderChatArea`）

1. tool_call 气泡：合并后续 tool_result 内容（`----` 分隔）；`\n[tool:` 分隔 aiText/toolTitle；正文 85% 宽；tool_result >5 行截断加 `...`；复制按钮。
2. reasoning-only 气泡：左缩进、带边框卡片；`[+]/[-] Reasoning (N chars)` 可折叠；展开 body 高度上限 6 行、滚动。
3. 普通气泡：user 右对齐 85% 宽；`[Context compacted:` → 黄色 + 顶部 `[Context compacted]` 标签；assistant 走 Markdown、其余纯文本；复制按钮。
4. 可见窗口：最后 100 气泡 + 顶部提示；`chatHistory` 上限 200。
5. 自动滚动。

### C. 状态机指示

- `[Agent: Idle|Streaming|ExecutingTools|AwaitApproval|Error]`。
- `ExecutingTool` → `| name (i/n)`。
- `Streaming` 且 `streamingText` 非空 → `| receiving SSE data...`。
- `AwaitApproval` → `| statusMessage`。

### D. 输入区状态行

- `Thinking`：spinner + `Reasoning... (N chars)`（仅 reasoning）或 `Generating... (N chars)` + `| reasoned N chars`。
- `ExecutingTool`：spinner + `Running: name (i/n)`。
- `AwaitingApproval`：spinner + `Awaiting approval...`。
- 其他：`statusMessage`；Idle：`Idle`。

### E. 配置弹窗

apiKey、baseUrl、maxTokens(512–65536)、temperature(0–2)、workspace、cpp、python；`Save & Connect` / `Cancel`。

### F. 审批弹窗

`YES -- Execute Once` / `ALWAYS -- Allow This Session` / `NO -- Cancel` + 破坏性命令列表。

### G. 状态栏

模型名、token（输入+输出）、消息数、工具数、上下文压力%。

### H. TODO 面板

标题 `TODO` + `pendingTodo` 逐行（`[x]`→done / `[*]`→in_progress / `[ ]`→open / 默认 pending）；空 → `(No active tasks)`；不渲染 overviewSummary（现状）。

### I. 菜单

File：New Chat `Ctrl+N` / Save As `Ctrl+S` / Open Chat `Ctrl+O` / Exit `Alt+F4`；Settings：Configuration / Theme / Customize...。

### J. 主题

`ThemeColors` 全字段 hex→颜色；`ThemeManager` 全方法；主题编辑器逐色调整 + 实时预览 + JSON 导入/导出；`onThemeChanged` 存 `config.toml`。

### K. 欢迎页

无 API key 时显示。

---

## 16. 附录 B：文件映射（旧 → 新）

| 旧文件 | 新文件（`src/tui/`） | 备注 |
|--------|----------------------|------|
| `main.cpp` | `main_tui.cpp` | FTXUI 主循环 |
| `gui_backend.cpp/h` | `terminal.cpp/h` | 终端初始化，删除 GLFW/OpenGL |
| `ui/app.cpp` | `app_tui.cpp/h` | 编排逻辑平移 |
| `ui/render_chat.cpp` | `chat_view.cpp` + `bubble_model.cpp` + `status_line.cpp` | 渲染/纯函数分离 |
| `ui/markdown_render.cpp` | `markdown_text.cpp/h` | 纯函数化 |
| `ui/render_settings.cpp` | `config_view.cpp` + `approval_view.cpp` | 弹窗拆分 |
| `ui/theme.cpp` | `theme_map.cpp` + `theme_manager.cpp` | 颜色映射 + 管理 |
| `ui/theme_popup.cpp` | `theme_editor_view.cpp` | 编辑器 |
| `ui/render_chat.cpp` 的 TODO 部分 | `todo_view.cpp` | 独立视图 |
| `ui/render_chat.cpp` 的欢迎页部分 | `welcome_view.cpp` | 独立视图 |

---

## 17. 风险与决策点

| 编号 | 事项 | 建议 | 影响 |
|------|------|------|------|
| R1 | FTXUI 中文/CJK 宽字符对齐、输入法（IME）支持弱 | Phase 1 即暴露并测试；必要时引入宽字符处理辅助 | 高，中文界面核心风险 |
| R2 | Windows 控制台 VT/UTF-8 环境差异（Windows Terminal vs 旧 cmd） | Phase 1 做 `SetConsoleOutputCP(CP_UTF8)` + VT 使能；文档明确最低支持 Windows Terminal | 中 |
| R3 | 终端无真正表格/图片，Markdown 排版退化 | Phase 4 定接受标准（表格折行/截断策略） | 中 |
| R4 | 长流式文本每帧重渲染性能 | 流式气泡用增量渲染/节流；保持 200 气泡上限 | 中 |
| R5 | 主题编辑器 116 处 ImGui 调用重写量大 | 独立 Phase 9，功能拆小步 | 高（工作量） |
| D1 | R2 语义：overview 归档是否要"展示保留但断开注入" | 本期采用彻底清空；如需归档展示另立任务 | 低 |
| D2 | FTXUI 是否纳入 git、`.gitignore` 处理 | Phase 1 决策 | 低 |
| D3 | 复制按钮在 TUI 的交互（无鼠标剪贴板） | 按键/菜单触发 + `SystemUtil` 写剪贴板 | 低 |
| D4 | 表格在终端下的接受标准 | Phase 4 明确 | 中 |
| D5 | TODO 面板是否新增 overview 展示 | 保持现状不新增 | 低 |
| D6 | 文件对话框从系统弹窗改为终端输入 | Phase 10 明确交互 | 低 |

---

## 18. 建议的提交节奏（迁移阶段）

每个 Phase 一个 commit（或一组聚焦 commit），提交信息示例：

```
feat(tui): Phase N -- <功能名> (1:1 移植 + 测试)
test(tui): add bubble_model / markdown_text unit tests
chore(tui): integrate FTXUI 7.0.3 into CMake
```

R2 修复（Phase 0）单独提交并立即 push，与后续迁移解耦，避免"一个小修复被大迁移阻塞在本地"。

---

## 19. 1:1 代码校验（全部阶段完成后执行）

迁移完成后执行一轮系统性代码校验，产出校验报告，确认无疏漏：

1. **行为清单逐条核对**：以附录 A 为基线，逐条确认 `src/tui/` 中有对应实现，逐条打勾记录。
2. **新旧文件映射核对**：按附录 B 逐文件 diff，确认每个旧文件的职责在新文件中完整落位、无功能遗漏。
3. **关键函数级对照**（重点）：
   - `deriveBubblesFromMessage` ↔ `bubble_model.cpp` 的 `deriveBubbles`
   - `formatToolMsg` ↔ `bubble_model.cpp` 的 `formatToolMsg`
   - `renderChatArea` ↔ `chat_view.cpp`
   - `renderStatusBar` ↔ `status_bar.cpp`
   - `renderTodoPanel` ↔ `todo_view.cpp`
   - `renderConfigPopup` / `renderToolApprovalDialog` ↔ `config_view.cpp` / `approval_view.cpp`
   - `renderMarkdown` ↔ `markdown_text.cpp`
   - `renderThemePopup` ↔ `theme_editor_view.cpp`
   - `renderWelcomePage` ↔ `welcome_view.cpp`
4. **残留引用扫描**：`grep` 确认 `ImGui` / `GLFW` / `imgui` 在新前端与 `CMakeLists.txt` 中无残留引用。
5. **校验报告**：输出三列清单（已覆盖 / 行为差异 / 未覆盖），行为差异必须引用 §17 对应决策点，未覆盖必须给出说明。

---

## 附录 C：手工待测项清单（最终人工验收，全部阶段完成后逐项勾选）

### C.0 Phase 0（R2 上下文清空）

- [ ] 真机复现：旧版完成一个带 TODO 的任务（`update_todo` init→update→done 归档）后执行 New Chat，确认 overview/TODO 不再进入新会话（修复前残留、修复后清空）。
- [ ] `/clear` 快捷命令同样清空 overview/TODO，且不破坏后续 `update_todo init` 生命周期。

### C.1 终端与中文（Phase 1）

- [ ] Windows Terminal 下中文输入/输出对齐正确、无错位。
- [ ] 中文输入法（IME）候选框与上屏正常（记录异常项）。
- [ ] 旧 cmd / ConHost 行为记录（不作为硬性门禁，仅记录）。

### C.2 聊天渲染（Phase 3）

- [ ] 加载同一会话 DB，新旧版逐屏对照气泡文本/顺序/折叠卡片一致。
- [ ] reasoning 折叠卡片 `[+]/[-]` 展开收起、6 行滚动。
- [ ] tool_call 合并、tool_result 截断、`[Context compacted]` 黄色样式。
- [ ] 自动滚动：新消息到达时聊天区自动滚到底部（frame + focus 锚点）。
- [ ] 复制：F8 复制最后一条 assistant 消息到剪贴板，粘贴验证内容一致。

### C.3 Markdown（Phase 4）

- [ ] 真实模型输出的 Markdown（表格/代码块/链接/列表）渲染可读、链接可打开。

### C.4 流式（Phase 5）

- [ ] 真实 API：reasoning 先流式出现，正文随后流式出现，逐字/逐块更新。
- [ ] 结束后无重复气泡、无闪烁、正确落为正式气泡。
- [ ] 长推理（`finish_reason=length` 自动恢复）场景下流式状态正确。

### C.5 输入 / 状态（Phase 6）

- [ ] prompt 选择器切换角色不丢上下文。
- [ ] 状态行 Thinking/ExecutingTool/AwaitApproval/Idle 各状态文本与旧版一致。
- [ ] 状态栏模型名/token/消息数/工具数/上下文压力与旧版一致。

### C.6 配置 / 审批 / 欢迎（Phase 8）

- [ ] Save & Connect 后 `config.toml` 落盘正确、工具用新 workspace 重注册。
- [ ] 审批弹窗三按钮（执行一次 / 本次总是 / 取消）行为正确。
- [ ] 无 API key 时欢迎页显示。

### C.7 主题（Phase 9）

- [ ] F6 主题菜单：Obsidian/Light + 6 套安装主题（artism_warm/colorblind_safe/cyber_punk/forest/monochrome_dark/vibrant_focus）可切换，颜色实时生效。
- [ ] F7 主题编辑器：字段列表滚动、hex 输入、预览色块、Apply 实时生效、Save 导出 JSON 到 projv_files/theme/。
- [ ] 主题切换后 chat/markdown/todo/status 颜色与旧版一致。

### C.8 菜单 / 会话 / 文件（Phase 10）

- [ ] Ctrl+N / Ctrl+S / Ctrl+O 快捷键、Exit。
- [ ] New Chat / Open Chat / Save As 会话切换正确（含 `lastMessageId_` 重置、气泡重建）。
- [ ] 文件对话框改终端输入路径（§17 D6）。

### C.9 project context 注入

- [ ] 发送消息后，检查会话 DB（projv_files/sessions/*.db）messages 表第二条为 `[PROJECT CONTEXT] Workspace directory structure...` system 消息，含工作区目录结构。

### C.10 双平台与终局

- [ ] Windows + Linux 双平台构建产物可运行。
- [ ] 全量 `proJV_tests` + golden 通过。
- [ ] 删除旧 `ui/` 后无遗留引用（`grep ImGui/GLFW` 为空）。
- [ ] §19 的 1:1 代码校验报告已输出并逐项勾选。

---

## §19 执行结果：1:1 代码校验报告（2026-09-11）

### 校验方法

- 逐条对照附录 A 行为清单，核对 `src/tui/` 实现。
- 关键函数级对照（旧 `src/ui/` ↔ 新 `src/tui/`）。
- `grep ImGui|GLFW|imgui` 残留扫描（后端 + 新前端 + CMake）。

### 已覆盖（✅）

| 行为 | 旧实现 | 新实现 | 测试 |
|------|--------|--------|------|
| 消息→气泡派生（system 过滤/tool 顺序/reasoning 前置） | `deriveBubblesFromMessage` | `bubble_model::deriveBubbles` | `test_bubble_model.cpp` |
| 工具消息格式化（参数提取/tool_result 摘要） | `formatToolMsg` | `bubble_model::formatToolMsg` | `test_bubble_model.cpp` |
| 气泡渲染（tool 合并/reasoning 卡片/compacted/窗口） | `renderChatArea` | `chat_view::renderBubbles` | `test_chat_render.cpp` |
| Markdown 解析（行内/标题/列表/表格/代码块/引用/HR） | `markdown_render.cpp` | `markdown_text::parseMarkdown` | `test_markdown_text.cpp` |
| 状态行文本 | `renderInputArea` 状态行 | `status_line::render` | `test_status_line.cpp` |
| 状态栏文本 | `renderStatusBar` | `status_bar::render` | `test_status_line.cpp` |
| TODO 面板 | `renderTodoPanel` | `todo_view::renderTodoPanel` | `test_todo_render.cpp` |
| 配置弹窗/欢迎页/审批弹窗 | `renderConfigPopup`/`renderWelcomePage`/`renderToolApprovalDialog` | `config_view` | `test_approval_logic.cpp` |
| 审批删除命令提取 | 内联在审批弹窗 | `approval_logic::extractDeleteFiles` | `test_approval_logic.cpp` |
| hex 颜色解析 | `ThemeColors::toVec4` | `theme_map::hexToColor` | `test_theme_map.cpp` |
| SSE 流式解析 | `parseSSEChunk` | 复用（后端未动） | `test_deepseek_sse.cpp` |
| R2 上下文清空 | `clearSession`（已修） | 复用 | `test_agent_request.cpp` |
| 流式实时显示 | 缺失（仅字符计数） | `renderStreamingBubble` + `BATCH_INTERVAL=1` | `test_chat_render.cpp`（streaming 用例） |

### 行为差异（已全部补齐，补充提交 4d6a495）

1. ✅ **自动滚动**：`chat_view` 加 `frame` + 底部 focus 锚点。
2. ✅ **相位指示器**：`main_tui` 渲染 `[Agent: Phase]` 行。
3. ✅ **菜单栏/快捷键可发现性**：底部帮助行（F2–F8）。
4. ✅ **主题切换 + 逐色编辑器**：`theme_editor_view`（F6 主题菜单 / F7 hex 编辑器）。
5. ✅ **复制按钮**：F8 复制最后一条 assistant 消息（`clipboard`）。
6. ✅ **文件对话框**：F4/F5 终端路径输入（范式替代）。
7. ✅ **project context 注入**：`core/project_context` + `app_tui` 注入。

### 测试覆盖（补齐后）

- 51 测试用例 / 234 断言全绿（doctest）。
- 新增：`theme_colors`（74 字段清单 + JSON round-trip）、`theme_map`、`approval_logic`。

### 残留引用扫描

- `src/tui/`：无 ImGui/GLFW 代码依赖（仅迁移说明注释）。
- 后端（`core/`、`client/`、`tools/`、`platform/`）：无 ImGui 依赖。
- `CMakeLists.txt`：无 imgui/glfw/OpenGL 目标残留。

### 测试覆盖

- 47 测试用例 / 154 断言全绿（doctest）。
- 单元：R2、气泡派生、Markdown、SSE、状态、审批、颜色。
- golden：FTXUI smoke、聊天渲染、TODO 面板。
