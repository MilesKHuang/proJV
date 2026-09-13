# proJV TUI 行为对照与修复清单（ImGui → FTXUI 迁移收尾）

> 状态：✅ 已全部解决（2026-09-13）。本清单保留作为验收记录；实现见 `RELEASE_NOTES_v0.5.0.md`。

- 日期：2026-09-12
- 分支：`dev-049`（当前 HEAD `a169c12`）
- 对照基线：旧 ImGui 实现 = `5820619^`（即 `0d03d19`，Phase 1–8 完成、删除 ImGui 之前）；新 FTXUI = 当前 `src/tui/`
- 状态：待实施

> ⚠️ 更新（2026-09-13）：下列 7 项 + A–I 排查项均已实现并合入 `dev-049`，对应改动见 `RELEASE_NOTES_v0.5.0.md`。保留原文作为设计/验收记录。

## 背景

前端从 Dear ImGui + GLFW/OpenGL3 迁移到 FTXUI（`external/FTXUI-7.0.3`）后，后端（`src/client`、`src/core`、`src/tools`、`src/platform`）保持冻结。对照旧 `src/ui/` 实现，发现若干移植遗漏或行为差异，本文档逐条记录现象、根因、修复方案与涉及文件，作为收尾实施的验收基线。

---

## 已知问题与修复方案

### 1. `/help` 等系统指令未集成

- **现象**：输入 `/help`、`/clear`、`/compress`、`/workspace` 无效果（会被当成普通消息发给模型）。
- **根因**：旧 `render_chat.cpp::trySend` 在 `startTurn` 前先走 `/workspace` 特殊处理和 `agent->handleQuickCommand()`（命中则短路）。新 `src/tui/app_tui.cpp::sendMessage` 直接 `agent->startTurn(text)`，两条短路逻辑全丢。`Agent::handleQuickCommand` 本身仍在后端（未删）。
- **方案**：`TuiApp::sendMessage` 恢复旧顺序：先 `/workspace`（前端直接产生 system bubble 显示路径），再 `handleQuickCommand`（命中则返回 true 短路并触发重绘），最后才 `startTurn` + 启动后台线程。`handleQuickCommand` 只保留 `/clear /help /compress`（`/save /load /doctor` 删除，见 C、I）。
- **补充**：欢迎页 `Commands: ...` 提示同步更新（去掉 `/save /load /doctor`）。
- **涉及文件**：`src/tui/app_tui.cpp`、`src/tui/app_tui.h`、`src/tui/main_tui.cpp`（重绘回调）。

### 2. system prompt 无法切换（角色切换）

- **现象**：无法在 coder/designer/analyzer 等角色间切换。
- **根因**：旧 `render_chat.cpp:590` 顶部有 ImGui Combo 列出 `promptFiles_`（隐藏 `compactor.md`），Idle 时选中即 `agent->replaceSystemPrompt(content)`。新 `app_tui.cpp` 只初始化 `promptFiles_/activePromptIndex_`，无 UI 入口，`replaceSystemPrompt` 未被 UI 调用。
- **方案（用户确认）**：在 **status bar 放一个带主题底色的高亮文字显示当前角色**，按 **Tab**（或其他键）循环切换。切换仅 Idle 生效（与旧逻辑一致），命中即 `replaceSystemPrompt`。需在 `TuiApp` 暴露 `promptFiles()/activePromptIndex()/switchPrompt(int)`。
- **注意**：Tab 与输入框焦点需统一处理（输入框聚焦时 Tab 不应切换角色，或在主界面统一拦截）。
- **涉及文件**：`src/tui/status_bar.cpp/h`、`src/tui/main_tui.cpp`、`src/tui/app_tui.cpp/h`。

### 3. F6/F7 必须用鼠标

- **现象**：F6（主题菜单）/ F7（主题编辑器）弹出后需鼠标点击才能操作。
- **根因**：FTXUI 的 `Menu` 组件吞掉 Tab 键（Tab 用于菜单项间循环），导致无法用 Tab 切到相邻 `Apply/Cancel` 按钮；`Menu` 的 Enter 只选中菜单项、不触发 Apply。所以 Apply 只能鼠标点。
- **方案（用户确认）**：**方向键选择 + Enter 应用/确认**。
  - F6 主题菜单：改用 `Radiobox`（或给 `Menu` 设 `on_enter` 直接应用），Esc 关闭，去掉 `Apply/Cancel` 按钮。
  - F7 主题编辑器：字段列表用 `Radiobox`（方向键 + Enter 确认字段），Hex 输入用 `Input`（Enter 确认），在容器 `CatchEvent` 里接管 Tab 在"字段列表 ↔ Hex 输入 ↔ 按钮"间切换焦点，绕开 `Menu` 的 Tab 捕获。
- **涉及文件**：`src/tui/theme_editor_view.cpp`。

### 4. Light 主题换不了浅色背景

- **现象**：切到 Light 主题，背景仍是终端黑。
- **根因**：旧 ImGui 靠 `ThemeColors::applyToImGui()` 把 `windowBg/menuBarBg/frameBg/text` 写入 ImGui Style，整窗随主题。新 FTXUI 渲染树完全没有应用背景色（`bgcolor` 只出现在 markdown 代码块与主题预览色块）。`theme_colors.cpp` 的 `light()` 数据齐全（`windowBg=#F5F5F5`、`text=#1A1A1E`），缺渲染层应用。
- **方案**：`main_tui.cpp` 根元素 `| bgcolor(hexToColor(T.windowBg))` 铺满整屏，并对 `chat_view/status_bar/todo_view` 中未显式着色的 `text(...)` 统一套 `color(hexToColor(T.text))`（否则浅底白字看不清）。主题切换后接 `onThemeChanged` 触发一次 `PostEvent` 重绘。
- **涉及文件**：`src/tui/main_tui.cpp`、`src/tui/chat_view.cpp`、`src/tui/status_bar.cpp`、`src/tui/todo_view.cpp`（可能）等。

### 5. Enter 后输入不立即显示

- **现象**：按 Enter 发送后，自己输入的文字不会马上出现。
- **根因**：用户消息持久化在 `Agent::run()`（**后台线程**）里 `addPersistedMessage(Message::User(...))`；前端 `syncChatFromAgent` 从 DB 读增量，且 FTXUI 只在有事件时重绘。ImGui 每帧渲染所以无感；FTXUI 下 Enter 触发的重绘常发生在后台线程写库之前，之后又无事件，直到第一个 token 流回才重绘。
- **方案**：把用户消息持久化从 `run()` 提前到 `startTurn()`（主线程同步）：`startTurn` 里直接 `addPersistedMessage(Message::User(text))`，删除 `run()` 里的 `if (hasUserInput_) { addPersistedMessage(...); }`。`hasUserInput_/queuedUserText_` 仅此两处引用，删除安全。
- **涉及文件**：`src/core/agent.cpp`（后端，唯一需要动的后端点，属于修复而非行为变更）。

### 6. 默认 1M 上下文

- **现象**：DeepSeek 新模型基本 1M 上下文，但默认只按 64K 处理。
- **根因**：`models.h::contextWindowForModel()` 对 `deepseek-v4-*` 已返回 1M，但 `deepseek-chat/reasoner/r1` 与未知模型 fallback 仍返回 64K；config 默认 `context_window=0`（自动检测）。
- **方案**：`contextWindowForModel` 旧模型映射与 fallback 统一提到 `1'048'576`。`config.toml` 已有 `context_window` 覆盖键（默认 0=自动），满足"config 可调"。
- **可选（UI 调整）**：`config_view` 配置弹窗新增 "Context Window" 输入框（旧版 UI 也没有，属新增），写入 `config.contextWindow` 并 `agent->setContextWindow()`。
- **涉及文件**：`src/client/models.h`；（可选）`src/tui/config_view.cpp`、`src/tui/app_tui.cpp`。

### 7. tool call 与 result 未合并显示

- **现象**：工具调用和其结果被拆成两个独立气泡。
- **根因**：旧 `render_chat.cpp:338` 渲染时把 `tool_call` 气泡与其后紧邻的多个 `tool_result` 合并成一个工具气泡（标题 + 结果用 `--------------------` 分隔）。新 `chat_view.cpp` 把它们渲染成两个独立块（`── Tool ──` / `── Result ──`）。
- **方案**：`chat_view::renderBubbles` 恢复旧合并逻辑：遇到 `tool_call` 向后吞并紧邻 `tool_result`，渲染成单个 Tool 块；孤立 `tool_result` 仍单独显示；沿用旧版"超 5 行折叠"截断。
- **涉及文件**：`src/tui/chat_view.cpp`。

---

## 建议实施顺序

1. **#5**（Enter 即时显示）——影响所有交互体感，改动最小、可单测。
2. **C（退出关 DB）+ #1（快捷命令）**——退出显式关库；恢复 `/help /clear /compress /workspace`，删除 `/save /load /doctor`。
3. **B + H（Cancel + 忙时 queue）**——长任务可中断、可排队。
4. **#3**（键盘化）——解决 F6/F7。
5. **#2**（角色切换）——status bar 高亮 + Tab 切换。
6. **#7**（tool 合并）——渲染层，可与 #5 并行。
7. **#4**（主题背景）——背景 + 前景 + 重绘，涉及面稍广。
8. **#6**（默认 1M）——映射 + 可选 UI 输入框。
9. **A（模型切换）+ G（复制）+ 其余次要项**——模型切换、复制编码、About、TODO 开关、成本估算。

---

## 功能性排查记录（对照旧 ImGui，2026-09-12）

逐条对照旧 `src/ui/app.cpp`（菜单栏/状态栏/欢迎页/主循环/TODO）、`src/ui/render_chat.cpp`（气泡/输入区）、`src/ui/render_settings.cpp`（配置/审批）、`src/ui/theme_popup.cpp`（主题编辑器）后，除上文 7 点外，另发现以下移植遗漏或行为差异（按重要性排序）。

### A. 模型切换 UI 完全缺失（重要）

- **旧行为**：`renderMainMenuBar` 的 Settings 菜单里有 Model combo（`DeepSeekClient::fetchModels()` 拉取 `availableModels`，切换时 `agent->setModel()` + `agent->setContextWindow(0)` 重新自动探测窗口）。
- **现状**：新 TUI 只在 `initialize` 里 `agent->setModel(config.model)`，**没有模型切换入口，也没有调用 `fetchModels`**（该后端函数仍存在）。
- **影响**：用户无法在 deepseek-chat / deepseek-reasoner / v4 系列之间切换。
- **建议**：配置弹窗或状态区加模型选择（键盘驱动），切换时同步 `setModel` + `setContextWindow(0)`。

### B. Cancel（中断当前 turn）缺失（重要）

- **旧行为**：`renderInputArea` 在 `isWaiting` 时显示 **Cancel 按钮**（`agent->cancel()`），并禁用其余 UI。
- **现状**：新 TUI 无 Cancel 入口；Esc 是退出程序而非中断 turn；`sendMessage` 里 `if (agentThreadRunning_) return;` 只是静默忽略新输入。
- **影响**：长任务/卡住时无法中断，只能退出。
- **建议**：忙时把 Esc 或某个键改为 `agent->cancel()`（空闲时 Esc 仍退出），或在输入区显示 Cancel。

### C. `/save` `/load` 删除 + 退出时未显式关闭 DB（bug）

- **决策**：`/save` `/load` 快捷命令无意义（会话本就自动持久化到 DB），连同后端 `saveRequested/loadRequested` 标志、`SAVE_TRIGGERED/LOAD_TRIGGERED` 提示、`QUICK_HELP`/欢迎页里的 `/save /load` 文案一并删除。`handleQuickCommand` 保留 `/clear /help /compress`。
- **F4/F5 处理（用户确认）**：F4（Save Chat As）删除；F5（Open Chat）改为扫描固定 `projv_files/sessions/` 目录列出 `.db` 会话文件，用选单（方向键选 + Enter 打开，同 #3 的 Radiobox 模式）替代手输路径，选单显示文件名/时间。
- **新增 bug（退出关闭 DB）**：`TuiApp::shutdown()` 未显式 `storage.closeDatabase()`，退出时仅依赖 `Storage` 析构（`main` 返回才触发），WAL 也未显式 checkpoint。方案：`shutdown()` 在 `agent->cancel()` + `joinAgentThread()` 之后显式 `storage.closeDatabase()`（关闭前可 `PRAGMA wal_checkpoint(TRUNCATE)` 合并 WAL）。
- **涉及文件**：`src/core/agent.cpp/h`（删 `/save /load` 及标志）、`src/tui/app_tui.cpp`（显式关闭 DB）、`src/tui/main_tui.cpp` + `src/tui/config_view.cpp`（删 F4 save_dialog，F5 改为会话列表选单）。

### D. About 缺失（次要）

- **决策**：简单写两句即可（版本 + 一句话简介），并入帮助行或 `/help` 输出，不单独弹窗。

### E. TODO Panel 开关（Ctrl+T）缺失（次要）

- **旧行为**：View → TODO Panel（Ctrl+T）可开关侧边栏。
- **现状**：TODO 面板固定显示在底部（`size(HEIGHT, LESS_THAN, 6)`），无开关。
- **建议**：加一个键位折叠/展开 TODO 面板。

### F. 成本估算（$）缺失（次要）

- **旧行为**：菜单栏右侧显示 token + 按 `modelPrices` 估算的成本 `$x.xx`。
- **现状**：`status_bar` 只显示 `tok: p+c`，无成本；`config.modelPrices` 已解析但未展示。
- **建议**：status_bar 末尾追加 `~$x.xx`。

### G. 复制功能（已删除）

- **决策（2026-09-12 后续）**：F8 复制与 Windows Terminal 的全屏键冲突，用户决定删除 F8 复制功能，后续 F9/F10/F11 前移为 F8/F9/F10。`clipboard.cpp` 的 UTF-16 编码修复已实现但暂无调用者（保留备用）。

### H. 忙时输入处理（次要）

- **决策**：忙时按 Enter **不丢弃**，把消息 queue 起来，当前 turn 结束后自动发送；输入框附近显示 queue 信息（如"已排队 N 条"）。与 B 项（Cancel）一并设计：忙时可 queue + 可 cancel。

### I. `/doctor` 删除

- **决策**：`/doctor` 从未实现，从欢迎页提示 `Commands: ...` 中删除。

> Markdown 渲染（code/bold/italic/link/table/header/hr/quote/bullet/ordered）经对照**新实现反而更完整**，非遗漏。
