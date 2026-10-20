# proJV Skill 触发与自主调用 -- 执行方案 v2

> v2 修订：触发语法从 `!<技能名>` 改回 `/` 域技能名直连；删除旧 `/skill <name> <topic>` 触发形式；未知名直接透传为普通消息。Agent 自主调用（`run_skill` 工具）与现有信息交互方式全部保留。

## 背景与目标

Skill 本质是**给 Agent 的说明书 + 操作手册**：固定流程，教 Agent 编排现有工具完成复杂成果，不携带运行时能力。

当前 `/skill <name> <topic>`（8 字符）需用户显式触发，目标分两级演进：

| 级别 | 触发方式 | 形态 |
|------|---------|------|
| 一级 | 用户 `/monica <内容>`（技能名直连，零冗余词） | 替换旧 `/skill`，命令即触发 |
| 二级 | **Agent 自主调用** `run_skill{name, topic}` 工具 | 对齐业界 Task/subagent 形态 |

**关键决策**：触发域定为 `/` 而非 `!`（`!` 与 C++ 的 `!=`/`!flag`/`if(!x)` 冲突，讨论代码必然误伤）；未知名直接透传为普通消息、绝无拦截报错；删除旧 `/skill <name> <topic>` 触发形式，仅保留 `/skill list` 作为安全列表入口。Agent 自主调用（`run_skill`）与用户显式触发走两套输出（见"关键架构约束"），互不干扰。

## 一级触发语法规范

```
/monica <topic>     # 触发技能，topic = <topic>
/bigbang <topic>    # 同上
/skill list         # 唯一保留的旧命令：列出已安装技能
```

### 判定链（`app_tui.cpp` `sendMessage()`）

`sendMessage()` 现有结构（190 行起）新增"技能名直连"分支，按以下顺序判定：

```
sendMessage(text)：
  ├─ /workspace（208 行）        → 原逻辑，优先级最高（不动）
  ├─ /compress（219 行）         → 原逻辑，优先级最高（不动）
  ├─ "/xxx ..."：取第一个 token
  │   ├─ token（大小写不敏感）∈ listSkills()
  │   │     ├─ "skill" → /skill list → 列技能（安全网）
  │   │     └─ 其他技能名 → startSkill(name, 剩余文本为 topic)，返回
  │   └─ token ∉ listSkills()    → 不拦截，继续往下走
  ├─ 下一个 token 是 "/"（// 注释）或 "."（./ 相对路径）→ 跳过技能解析（实际已被"未命中透传"兜住）
  ├─ /clear /help                → handleQuickCommand 原逻辑（不动）
  └─ 普通消息                    → agent->startTurn()
```

要点：

- **大小写不敏感匹配**：`/Monica D:\dir` 与 `/monica D:\dir` 等价（ASCII 小写映射，技能名均为 ASCII）。
- **topic 为空**：`/monica` 单独发送时按普通消息透传，或提示 `[skill] usage: /monica <topic>`（沿用现有 startSkill 的 empty-topic 处理，`app_tui.cpp` 245-250 行）。
- **触发即持久化**：startSkill 里 `agent->addPersistedMessage(Message::User(...))`（307 行）持久化**用户实际输入**（如 `/monica D:\dir`），与历史重放显示一致。

### 与 vision `@` 共存

同一消息中 `@<路径>` 图像在触发技能时**忽略并提示**（系统气泡："图像已忽略，skill 暂不支持携带图像"），不静默丢弃。规则不变。

**判定顺序**：`sendMessage()` 中技能直连分支（`/` 前缀）**先于** vision 的 `@` 解析分支执行。即 `/monica D:\dir @C:\a.png` 命中技能后直接进入 `startSkill`，`@` 图像标记不进入文本剥离流程（由 startSkill 的 image-ignore 提示兜底）；未命中技能走 `@` 解析。两分支互不干扰，实现时保持此顺序。

## 关键架构约束

### 1. 复用运行时而非状态机

`run_skill` 作为普通工具注册，executor 同步阻塞在 `ExecutingTools`；SkillRunner 保持自己的线程/上下文，与主 Agent 仅通过 `ToolResult.content` 交汇。Agent 状态机**零改动**——结果回填后 `Agent::run()` 由 `ExecutingTools` 自动转回 `Streaming`（`agent.cpp` 313 行既有逻辑）。工具执行期间 `agentThreadRunning_` 为 true → 用户输入自动排队（`app_tui.cpp` 194-197 行）、busy 标志与 spinner 自动亮、取消入口保持唯一（ESC）。

### 2. 两条路径的输出模式（硬性区分）

| 路径 | 触发 | 输出 |
|------|------|------|
| 用户显式 `/monica <dir>` | `TuiApp::startSkill` | cbs.onEvent 逐条流式入主会话（**保留现有交互**） |
| Agent 自主 `run_skill` | executor 同步 run() | **onEvent 不流式入会话**（会破坏 tool_calls 配对，触发 repair/validate 误判）；只把 `lastDoc_`（emit=document 产出）作为工具返回值 |

### 3. 取消链 = probe 回调注入（run_skill 专用）

SkillRunner 在既有回合/action 边界检查点（`skill_runner.cpp` 236/239 行 `run()` 主循环、257 行 `executeSteps()`）追加探测 `toolRegistry.isCancelled()`；ESC → `Agent::cancel()` → `cancelAll()` 置标志 → SkillRunner 下一边界探测到 → `cancel()` 打断 sub-agent → 退出。零新线程，最坏等一个 LLM 往返。

> **为什么需要这个桥接**：显式 `/monica` 路径下 `TuiApp` 全局持有 `skillRunner_`（352 行），ESC → `cancelTurn()`（374 行）直接可达，无需改动。`run_skill` 注册后，SkillRunner 实例变为 executor 局部变量，ESC 的取消链只剩 `Agent::cancel()` → `toolRegistry.cancelAll()`，够不到 SkillRunner —— 故 `run_skill` 实现时必须接 `setCancelProbe(tools.isCancelled())`，否则该路径 ESC 失效（静默退化）。

## 实施步骤

### Phase 1：触发词 `/<技能名>`

1. **`app_tui.cpp` `sendMessage()`**（190-263 行）：
   - 在 `/workspace`、`/compress` 判定之后，新增技能名直连分支：取第一个 token，大小写不敏感匹配 `SkillRunner::listSkills(skillsDir_)`；命中即调 `startSkill(name, 剩余文本)` 并返回。
   - 大小写归一：`/Monica` → `monica`，用 ASCII 小写映射（技能名均为 ASCII）。
   - **删除**旧 `/skill <name> <topic>` 分支（227-253 行），仅保留 `rest == "list"` 的分支（列技能，安全网）。
   - `startSkill()`（300-364 行）：持久化的用户消息改为实际输入文本（而非拼 `/skill ...`），其余零改动。
2. **`agent.cpp` QUICK_HELP**（35-40 行）：`/skill <name> <topic>` 行改为 `/monica <topic>` 示例，并保留 `/skill list`。
3. **README 快捷键表**：`/skill <name> <topic>` → `/monica <topic>`；快捷命令列表同步。

### Phase 2：注册 `run_skill` 工具（Agent 自主调用核心）

4. **`app_tui.cpp` `setupTools()`**（28-48 行）：注册 `ToolDefinition{run_skill, name, topic}`，executor lambda 捕获 `this`：
   - 解析参数 → 校验模型支持工具（`modelSupportsTools`，`models.h` 132-134 行）与技能存在（`SkillRunner::listSkills`）
   - 现拼 `sharedContext`（复用 332-350 行主 Session 摘要逻辑）
   - 构造 `SkillRunner`（复用 352 行 `config/tools/cbs`），**cbs.onEvent 留空或仅更新 `skillStatus_`**，不调 `agent->addPersistedMessage`
   - `setCancelProbe([&tools]{ return tools.isCancelled(); })`
   - 同步 `run()` → 返回 `lastDoc_`（空则返回运行状态摘要）
5. **`skill_runner.h/.cpp`**：新增 `setCancelProbe(std::function<bool()>)`；在 `run()` 主循环（236/239 行）与 `executeSteps()`（257 行）的现有 `cancelRequested_` 检查点追加 probe 判断，命中时调 `cancel()` 并中断。
6. **`SkillRunner::cancel()` 已满足**（`skill_runner.cpp` 105-109 行遍历 sub-agents 调 `client_.cancel()`），无需改动。

### Phase 3：工具白名单与权限

7. **`agent.h` allowedTools 机制**（84-86 行）：`parseAvailableTools(systemPrompt_)` 决定模型可见工具集——`run_skill` 默认对所有角色开放；designer 等只读角色由 `coder.md`/`designer.md` 的 `[TOOL_CALL` 声明决定（沿用现有能力裁剪，不特判）。

### Phase 4：轻量 skill 自主（可选增强）

8. **说明书注入**：新增 `list_skills` / `read_skill` 工具，或把已安装技能的一行描述（`config.json` 的 name/description）注入 System Prompt；模型"知道自己有哪些说明书"后自主编排既有工具执行。评估后再决定是否做（Phase 2 已覆盖"Agent 自主调 SkillRunner"）。

### Phase 5：测试

9. 单测：
   - `/` 前缀解析：`/monica D:\dir` 命中触发、`/Monica` 大小写、`/skill list` 列技能、未知名（`/mica`）**透传为普通消息**、`/clear` `/help` 不被技能分支劫持
   - `run_skill` 工具定义与参数校验
   - cancel probe：注入 fake probe 断言 SkillRunner 在边界退出
   - 工具路径下 `onEvent` 不写主会话（黄金断言：ToolResult 仅含 lastDoc）
10. `clean_and_build.bat` 全量构建回归（含既有 vision Phase 1 改动）。

## 风险与对策

| 风险 | 对策 |
|------|------|
| skill 运行数分钟无反馈 | 状态行复用 `skillStatus_` + `onProgress` 更新（现成链路）；接受无流式 |
| 取消探测在 LLM 往返内不即时 | 回合边界轮询，最坏等一轮；符合"停止辩论"预期 |
| `run_skill` 与用户显式 `/monica` 并发 | 工具路径天然占用 `agentThreadRunning_`，显式触发会 queue——复用现有排他 |
| 模型误用 run_skill | 工具描述写清"重型多 Agent 流程，昂贵耗时，仅在明确复杂任务时使用" |
| `/` 误伤自然语言（如 `/usr/...` 路径） | 未命中透传兜底 + 技能名=已安装列表，命中触发已足够精确；`//` 注释、`./` 相对路径首 token 天然不匹配技能名 |
| 用户不知可触发哪些技能 | `/skill list` 安全网 + `/help` 列出已安装技能 |

---

## 实施记录（v0.5.7）

状态：**Phase 1 / 2 / 3 完成，Phase 5 部分完成，Phase 4（可选）未做**。提交 `1b9db1f`。

### 代码落点

| 文件 | 内容 |
|---|---|
| `src/core/skill_trigger.h/.cpp` | 新增。`/<token>` 判定为纯函数（None / List / Trigger），供 `sendMessage()` 调用并被单测覆盖 |
| `src/tui/app_tui.cpp` | `sendMessage()` 技能分支改为调用 `skill_trigger::parse`；`startSkill(name, topic, rawInput)`；`run_skill` 工具注册；抽出 `buildSharedContext()` |
| `src/core/skill_runner.h/.cpp` | `setCancelProbe()` + `checkCancel()`；`run()`、`executeSteps()` 的 round/step/action 边界检查 |
| `src/core/prompts_loader.cpp` | `coder.md` 默认工具表加入 `run_skill`，并对旧种子文件做一次性迁移 |
| `tests/unit/test_skill_trigger.cpp` | 触发器规则 7 组 + cancel probe 2 组 + "无事件仍产出 lastDoc" 1 组 |

### 实施决策（文档留白处，均已可改）

1. **保留字保护**：`/clear` `/help` `/workspace` `/compress` 即使被同名技能文件夹遮蔽也按原命令处理（本文档只写了"未命中透传"）。
2. **topic 为空**：选本文档给出的第二种做法 —— 系统气泡 `[skill] usage: /<name> <topic>`，不发送。
3. **`/skill <其他>`**：一律当列表输出（`/skill list` 的安全网），不做拦截报错。
4. **`run_skill` 返回值**：`lastDoc_` 非空则返回文档；否则返回 `[skill] <name> finished after N round(s), converged=yes/no, but produced no document`；被取消返回 `cancelled before producing a document`。不重试。
5. **`run_skill` 不发事件**：`cbs.onEvent` 留空，仅 `onProgress` 更新 `skillStatus_`（状态行）。SkillRunner 用 executor 局部对象，**不**发布到 `skillRunner_`，因此右侧 Agent 面板在 `run_skill` 路径下为空（避免跨线程访问 `agents_`）。显式 `/<skill>` 路径的面板行为不变。
6. **技能名解析复用**：`run_skill` 用 `skill_trigger::parse("/" + name, installed)` 解析，与用户触发共用同一套规则（大小写不敏感、容忍模型多写的 `/`）。
7. **提示词迁移**：只在 `coder.md` 的工具行**逐字等于旧默认值**时追加 `run_skill`（幂等，可单测）；手改过的 prompt 不动。

### 与 01_IMAGE_INPUT 的冲突已裁决

01 文档仍写 `!<技能名>` 触发，本文档 v2 改为 `/`。实现按 **本文档 v2**：`!` 不参与任何触发，`@` 图像在技能触发时忽略并提示。

### 已知缺口

- `/help` 不列出已安装技能（本文档风险表中的缓解手段，非实施步骤）。需要的话可在 `handleQuickCommand` 里注入技能列表。
- `run_skill` 的 executor 本体（在 `TuiApp::setupTools`）没有直接单测，只有其依赖的两条不变量被测试覆盖（触发器规则、runner 无事件产出）。
