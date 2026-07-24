# proJV v0.4.6 重构

> 2026-07-24 | 设计阶段

---

## 1. 目标

| # | 需求 |
|---|------|
| 1 | supervisor / tester / coder → 全能 coder（全工具，默认）。保留 designer / analyzer / compactor |
| 2 | cancel 时中断 LLM 流 + 终止正在执行的 tool 子进程，立即回到 Idle |
| 3 | `projv_*` 目录 + `config.toml` → `projv/` 子目录 |

---

## 2. 角色合并

### 2.1 合并策略

```
supervisor (全工具) ─┐
coder       (编码)  ─┼──→ coder (全工具, 默认)
tester      (测试)  ─┘
designer / analyzer / compactor → 不变
```

### 2.2 工具矩阵

| 工具 | coder | designer | analyzer | compactor |
|------|:-----:|:--------:|:--------:|:---------:|
| 全量 11 个工具 | ✅ | - | - | - |
| read_file / grep_files / md_file | - | ✅ | ✅ | - |
| read_file / grep_files / file_search / md_file / diagram_tool | - | - | ✅ | - |

### 2.3 coder prompt 要点

- 定位：全工具（含 web_search、diagram_tool）
- Hard Rules：沿用 coder 现有规则
- WORKFLOW：保留 update_todo 四阶段（INIT → EXECUTE → VERIFY → DONE）
- Testing：追加测试流程（写测试、编译运行、PASS/FAIL 报告、失败修复）
- Design & Analysis：追加设计分析（读设计文档、diagram_tool、md_file）
- tools available 行：全量 11 个工具

### 2.4 UI

下拉框 `analyzer → coder → designer`，coder 默认，compactor 隐藏。

### 2.5 改动

- `prompts_loader.cpp`：presets[] 移除 supervisor / tester 条目；`PROMPT_DEFAULT_CODER` 常量更新为合并版内容；删除 `PROMPT_DEFAULT_SUPERVISOR` / `PROMPT_DEFAULT_TESTER` 两个 static constexpr
- `app.cpp` 第 237 行：`"supervisor.md"` → `"coder.md"`

---

## 3. Cancel 彻底化

### 3.1 问题

`ExecutingTools` 阶段逐 tool 同步执行，cancel flag 仅在两 tool 之间检查。`exec_shell` 阻塞数十秒时 Cancel 无效。

### 3.2 方案

**ToolRegistry 新增**（`registry.h/cpp`）：

| 方法 | 功能 |
|------|------|
| `cancelAll()` | 设置原子 cancel flag + 取 mutex 读 `activeProcessHandle_`，若非空则 TerminateProcess |
| `isCancelled()` | 返回原子 flag |
| `resetCancel()` | 新回合重置 flag |

新增成员：`std::atomic<bool> cancelRequested_`、`std::mutex activeProcessMutex_`、`void* activeProcessHandle_`。

**shell_tool 改造**（`shell_tool.cpp`）：

- `registerShellTool()` 的 lambda 当前捕获 `[workspacePath]`，需改为捕获 `[&registry, workspacePath]`，将 `&registry` 传入 `execCommand()`
- `execCommand()` 新增 `ToolRegistry*` 参数：
    1. 子进程创建后（`pi.hProcess` 有效时），取 mutex 写入 `registry->activeProcessHandle_`
    2. 等待循环改造——**cancel 检测与空闲超时解耦**：

**常量**：
- `kPollIntervalMs = 100`（原 3000）— cancel 响应延迟 ≤100ms
- `kMaxIdleMs = 30000`（新增）— 空闲超时维持 30s

**循环逻辑**：
1. `WaitForSingleObject(pi.hProcess, 100)` — 100ms 超时
2. 进程退出 → break
3. 检查 `registry->isCancelled()` → 若 true，TerminateProcess，`forcedKill=true`，break
4. 检查 reader 是否有新输出（对比 `output.size()` 与上次记录）：
        - 有新输出 → 更新 `lastOutputTime`（`steady_clock::now()`），continue
        - 无新输出 → 检查 `now - lastOutputTime > kMaxIdleMs`：超过 30s → 判定超时，`forcedKill=true`，break；未超过 → continue
5. reader 已完成但进程未退出 → 给一次短等待（2-3s），仍未退出则 `forcedKill=true`，break

循环结束后（无论正常/强制/超时），取 mutex 将 `activeProcessHandle_` 置空。

**设计理由**：100ms 轮询保证 Cancel 响应及时；空闲超时改用绝对时间戳（`lastOutputTime`）而非 `idlePolls` 计数，避免短间隔导致误杀（原 `3000ms × 10次 = 30s`，若直接改间隔会变成 `100ms × 10次 = 1s`）。

**Agent 改造**（`agent.cpp`）：

- `cancel()` 第 293 行后新增 `toolRegistry.cancelAll();`
- `newTurn()` 第 300 行后新增 `toolRegistry.resetCancel();`
- `ExecutingTools` 循环（第 204 行 while）中，`executeTool()` 返回后检查 `result.isCancelled`，若 true 则 break
- `executeTool()` 返回的 `ToolResult` 中，`exec_shell` 被 cancel 时设 `isCancelled=true`
- **Cancel 后清理孤儿 tool call**：`ExecutingTools` 被 cancel 中断时，session 中已有 assistant 消息（带 tool_calls）但无对应 tool result。在 break 进入 Idle 前调用 `session.stripOrphanedToolCalls()` 清理，否则下一轮 `buildChatRequest()` 会发送孤儿 tool call 导致 API 报错

**ToolResult 新增字段**（`models.h` 第 37-42 行）：

`bool isCancelled = false;`

**时序**：

```
Cancel → Agent::cancel()
  ├─ cancelRequested_ = true
  ├─ client.cancel()
  ├─ toolRegistry.cancelAll()  → 终止 shell 子进程
  └─ 唤醒 approvalCv_

run() 三阶段均检测 cancel → break → Idle
```

### 3.3 线程安全

两条线程：UI 线程（Cancel）↔ Agent 工作线程（执行 tool）。

| 状态 | 保护 | 结论 |
|------|------|------|
| `ToolRegistry::cancelRequested_` | `atomic<bool>` release/acquire | 安全 |
| `activeProcessHandle_` | `mutex`，读写均在锁内 | 安全 |
| `Agent::cancelRequested_` | 已有 `atomic<bool>`，不变 | 安全 |

唯一竞态：Cancel 时子进程尚未创建 → `cancelAll()` 看到空句柄不误杀 → Agent 线程创建进程后首次轮询（100ms 内）检测到 flag 自行终止。延迟 ≤100ms，可接受。

---

## 4. 路径统一

### 4.1 映射

| 旧 | 新 |
|----|----|
| `{exeDir}/config.toml` | `{exeDir}/projv_files/config.toml` |
| `{exeDir}/projv_theme/` | `{exeDir}/projv_files/theme/` |
| `{exeDir}/projv_prompts/` | `{exeDir}/projv_files/prompts/` |
| `{exeDir}/projv_pytool/` | `{exeDir}/projv_files/pytool/` |
| `{exeDir}/projv_sessions/` | `{exeDir}/projv_files/sessions/` |

### 4.2 实现

**config.h 新增声明**：

| 函数 | 返回 |
|------|------|
| `getExeDir()` | exe 所在目录 |
| `getProjvDir()` | `{exeDir}/projv_files` |
| `getThemeDir()` | `{exeDir}/projv_files/theme` |
| `getPromptsDir()` | `{exeDir}/projv_files/prompts` |
| `getPytoolDir()` | `{exeDir}/projv_files/pytool` |
| `getSessionsDir()` | `{exeDir}/projv_files/sessions` |

`getExeDir()` 通过 `GetModuleFileNameA` + `.parent_path()` 实现。其余基于 `getExeDir()` 拼接。

**config.cpp 修改**：

- `getConfigPath()` 第 17 行：`"config.toml"` → `"projv_files/config.toml"`
- 新增 `migrateLegacyPaths()`：遍历 5 条映射，旧路径存在且新路径不存在 → rename。幂等。在 `App::initialize()` 第 206 行 `loadTomlConfig()` 之前调用
- 实现上述 6 个路径函数

**各模块引用替换**：

| 文件 | 位置 | 改动 |
|------|------|------|
| `prompts_loader.cpp` | 第 196-198 行 `promptsDir()` | `getConfigPath().parent_path() / "projv_prompts"` → `getPromptsDir()` |
| `python_tool_manager.cpp` | 第 18-20 行 `pytoolDir()` | `getConfigPath().parent_path() / "projv_pytool"` → `getPytoolDir()` |
| `theme.cpp` | 第 202 行 `ThemeManager::init()` | 参数 `exeDir` 语义改为直接接收 theme 目录路径；`exeDir+"/projv_theme"` → 直接使用入参 |
| `main.cpp` | 第 308 行 | `ThemeManager::instance().init(exeDirStr, ...)` → `init(getThemeDir(), ...)` |
| `app.cpp` | 第 187 行 workspace fallback | `getConfigPath().parent_path()` → `getExeDir()` |
| `app.cpp` | 第 249 行 sessions 路径 | `exeDir + "/projv_sessions"` → `getSessionsDir()`（原 `exeDir` 局部变量不再使用，可删除） |
| `render_chat.cpp` | 第 683、720 行 workspace fallback | `getConfigPath().parent_path()` → `getExeDir()` |

> **关键**：`getConfigPath()` 改为返回 `projv_files/config.toml` 后，`.parent_path()` 得到的是 `projv_files/` 而非 exe 目录。所有依赖此行为获取 exe 目录的代码（workspace fallback）必须改用 `getExeDir()`。

---

## 5. 改动清单

| # | 文件 | 操作 | 说明 |
|---|------|------|------|
| 1 | `src/core/prompts_loader.cpp` | 修改 | presets[] 移除 supervisor/tester；`PROMPT_DEFAULT_CODER` 更新为合并版；删除 `PROMPT_DEFAULT_SUPERVISOR` / `PROMPT_DEFAULT_TESTER` |
| 2 | `src/ui/app.cpp` | 修改 | L237 `"supervisor.md"`→`"coder.md"`；L187 workspace fallback 改用 `getExeDir()`；L249 sessions 改用 `getSessionsDir()`；L206 前调用 `migrateLegacyPaths()` |
| 3 | `src/tools/registry.h` | 修改 | 新增 `cancelAll()` / `isCancelled()` / `resetCancel()` 声明；新增 `cancelRequested_` / `activeProcessMutex_` / `activeProcessHandle_` 成员 |
| 4 | `src/tools/registry.cpp` | 修改 | 实现 `cancelAll()`（设 flag + 终止进程 + 清句柄）、`isCancelled()`、`resetCancel()` |
| 5 | `src/tools/shell_tool.cpp` | 修改 | lambda 捕获 `&registry`；`execCommand()` 新增 `ToolRegistry*` 参数：注册句柄、100ms 轮询 cancel + 绝对时间戳空闲超时（30s）、终止进程、退出后清句柄 |
| 6 | `src/core/agent.cpp` | 修改 | `cancel()` 调用 `toolRegistry.cancelAll()`；`ExecutingTools` 检测 `result.isCancelled` break；`newTurn()` 调用 `toolRegistry.resetCancel()` |
| 7 | `src/client/models.h` | 修改 | `ToolResult` 新增 `bool isCancelled = false;` |
| 8 | `src/core/config.h` | 修改 | 新增 `getExeDir()` / `getProjvDir()` / `getThemeDir()` / `getPromptsDir()` / `getPytoolDir()` / `getSessionsDir()` 声明 |
| 9 | `src/core/config.cpp` | 修改 | `getConfigPath()` → `projv/config.toml`；实现 6 个路径函数 + `migrateLegacyPaths()` |
| 10 | `src/core/prompts_loader.cpp` | 修改 | `promptsDir()` 改用 `getPromptsDir()` |
| 11 | `src/tools/python_tool_manager.cpp` | 修改 | `pytoolDir()` 改用 `getPytoolDir()` |
| 12 | `src/ui/theme.cpp` | 修改 | `ThemeManager::init()` 参数语义改为直接接收 theme 目录，不再拼接 `/projv_theme` |
| 13 | `src/main.cpp` | 修改 | `ThemeManager::init(exeDirStr, ...)` → `init(getThemeDir(), ...)` |
| 14 | `src/ui/render_chat.cpp` | 修改 | L683、L720 workspace fallback 改用 `getExeDir()` |

---

## 6. 风险

- **角色合并**：旧 supervisor.md / tester.md 不主动删除，用户手动选择仍可加载但不在 presets 列表
- **Cancel**：强制终止子进程不执行清理（临时文件 OS 回收）；非 shell 工具执行极短无需 cancel；Python 工具子进程同样被终止
- **路径迁移**：rename 而非复制，幂等；失败不阻塞启动，旧路径保留
- **getConfigPath() 语义变更**：返回值从 `{exeDir}/config.toml` 变为 `{exeDir}/projv/config.toml`，`.parent_path()` 不再等于 exe 目录。必须全局搜索 `.parent_path()` 调用点，替换为 `getExeDir()` 或对应 `getXxxDir()`