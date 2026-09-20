# SKILL_ENGINE — 多 Agent 工作流引擎设计 (v1.1)

> 目的: 一个扩展点 -- 丢一个 JSON(+prompts/tools) 即得一套多 Agent 工作流, 零 C++ 改动. 覆盖 圆桌 / 专家会议 / 头脑风暴 / Kanban / 蜂群 这一族, 以**统一、简化、有限**的方式实现; 与现有 `/bigbang` **正常路径逐字节兼容**.
> 关系: `bigbang_debate` 是本引擎的**内置技能 #1**, 不是特例.
> v1.1: 并入 review 修复 (Bug A/B + C-G + 派生式工具注册 + 事件文字规范). 本文件自包含, 不依赖任何旧文档.

---

## 0. 设计命题 (why this shape)

不造通用工作流引擎 (DAG/Turing). 造 **黑板 + 程序 + 停止规则** 三件套; 多样性住在配置里, 引擎只是一台小解释器.

| 通用引擎会要的能力 | 代价 | 本族是否需要 |
|--------------------|------|-------------|
| 动态列表 map/reduce | 类型系统 + 求值器 | 否 |
| 基于内容的 if/分支 | 表达式引擎 | 否 |
| 全局可变黑板 / 事务 | 并发模型 | 否 |
| 异步任务队列 | 调度器 + 生命周期 | 否 |

把引擎压到最小: 可审计 (~250 行)、可预测、每个工作流都是数据. 这不是妥协, 是设计选择.

---

## 1. 抽象模型 (五件套)

```
 skill.json
    |
    v
 [SkillRunner] --owns--> Blackboard { slot -> text | agentId -> VoteResult }
    |  loads
    +----> roles: map<id, SubAgent(DeepSeekClient + Session + toolDefs)>
    |
    for round in 1..max_rounds:                # 唯一控制结构
        for step in round_steps:               # 程序
            actions (parallel | seq), gated by `when`
            merge: capture -> slot / votes ; emit -> onEvent -> TUI
        if STOP(rule): break
    run on_converge  -> document event          # 后程序
```

| 对象 | 职责 | 实现 |
|------|------|------|
| Role | LLM 角色: id + prompt + 工具 | `SubAgent` |
| Blackboard | 文本槽 `map<slot,string>` + 投票 `map<agentId,VoteResult>`; 步骤间唯一通道 | `SkillRunner` |
| Step group | 一组 call, 并行/顺序, `when` 门控 | `SkillStep` |
| Call (action) | role + tool + message + capture/into/shape/emit | `SkillAction` |
| Program + Stop | `round_steps` 每轮一遍, 直到 stop 或 max_rounds | `run()` |
| Post program | `on_converge` (产出 doc) | `run()` |
| Events | `onEvent(role, kind, text)` 流到 TUI | `Callbacks` |

### 1.1 类型系统只有两种形状

| shape | 内容 | 引擎用途 |
|-------|------|---------|
| `text` | 一个字符串 | 发言 / 汇总 (绝大多数) |
| `vote` | agree / agreed_points / concerns / suggested_tweak | 收敛判定 |

**引擎不出现 `statement` / `agree` / `doc_markdown` / `bigbang` 任何一个字** -- 字段名、工具名、文案全部来自配置. 这是"换配置即换场景"成立的前提.

### 1.2 黑板 = 命名槽 (核心接口)

- 步骤**不互相直接引用**, 只读写命名槽/投票区.
- 文本: `into` 决定写哪个槽 (默认 = agent id); 模板 `{{slot.current}}` / `{{slot.prev}}` 读.
- 投票: 按**投票者 agent id** 存 (`votes_[agentId]`), **不写文本槽** (见 §4.3 Bug-A).
- kanban 的 `task1/task2`、脑暴汇聚、专家会汇总, 全部复用同一文本槽机制.
- `{{round_outputs}}` = 本轮所有 text 捕获的拼接 (供 synthesizer 一次读全场).

---

## 2. 覆盖矩阵与"有限"边界

| 工作流 | 程序 (`round_steps`) | 停止 | 轮次 | 状态 |
|--------|---------------------|------|------|------|
| 圆桌 (bigbang) | 提案 -> 整合 -> 投票 | `all_agree` | 多轮 | 内置, 正常路径逐字节兼容 |
| 专家会议 | N 专家并行发言 -> 主持汇总 | `none` | 1 | 直接支持 |
| 头脑风暴 | N 发散并行 -> 聚类 | `none` | 1 | 直接支持 |
| Kanban | 计划 -> 分工(每 worker 不同 `into`) -> 复核 | `all_agree` \| `none` | 多轮=重计划 | 支持 |
| 蜂群 | `dynamic_dispatch` + `request_agent` | `none` | max_rounds 兜底 | 有限 |

明确**不做** (诚实边界, 属未来): 动态列表 map/reduce、内容条件分支、全局可变黑板(仅命名槽赋值)、异步/任务队列.

---

## 3. DSL 规范

### 3.1 顶层

| 字段 | 类型 | 必填 | 默认 | 说明 |
|------|------|------|------|------|
| `name` / `description` | string | 是 | - | |
| `tools` | array\<string\> | 否 | [] | **能力工具**白名单 (read_file 等, 只读建议). 发言工具不在此列 |
| `agents` | array | 是 | - | 见 3.2 |
| `max_rounds` | int | 是 | - | |
| `converge` | string | 否 | `"none"` | `all_agree` \| `none` |
| `loop_detect` | string | 否 | `""` | 文本槽名; 该槽本/上轮相同则提前停 |
| `dynamic_dispatch` | bool | 否 | false | |
| `max_recursion` | int | 否 | 3 | |
| `round_steps` | array | 是 | - | 程序 |
| `on_converge` | array | 否 | [] | 后程序 |

> **两类工具**: (1) **能力工具** = 真干活 (read_file / pytool 脚本), 由 `tools` 白名单控制, 经 `ToolRegistry` 执行; (2) **发言工具** = 只是"让模型按结构吐参数"的壳, 无执行体, 定义在 `tools/*.json`, 由引擎按 action 自动注册给对应 agent. **定义来源**: 能力工具取自 `ToolRegistry::getToolDefinitions()`; 发言工具取自 `tools/*.json`. `pytool` 属第 1 类, 完全不受影响.

### 3.2 agent

| 字段 | 类型 | 必填 | 默认 |
|------|------|------|------|
| `id` | string | 是 | - |
| `display_name` | string | 是 | - |
| `prompt` | string | 是 | - | 相对技能目录 |

> v1.1 删除 `can_write_doc`: 每个 agent 拿哪些**发言工具**, 由程序里 `(agent, tool)` 对**自动派生** (见 §4.3 注册). 少一个字段 = 少一类不一致 bug.

### 3.3 step group

| 字段 | 类型 | 必填 | 默认 |
|------|------|------|------|
| `when` | string | 否 | `""` (`""` \| `round==1` \| `round>1`) |
| `parallel` | bool | 否 | false |
| `progress` | string | 否 | `""` (onProgress 文本) |
| `actions` | array | 是 | - |

### 3.4 action (通用核心, 去硬编码的关键)

| 字段 | 类型 | 必填 | 默认 | 说明 |
|------|------|------|------|------|
| `agent` | string | 是 | - | role id |
| `tool` | string | 是 | - | 发言工具名 (来自 tools/*.json) |
| `message` | string | 是 | - | 模板 |
| `capture` | string | 否 | `""` | 取 args 哪个字段 (`shape=text`); 空则取整个 args JSON |
| `into` | string | 否 | =`agent` | 文本槽名 (`shape=text` 生效) |
| `shape` | string | 否 | `"text"` | `text` \| `vote` |
| `emit` | string | 否 | `""` | `""` \| `message` \| `vote` \| `document` |
| `vars` | object | 否 | `{}` | 本 action 模板局部变量 |

> `shape=vote`: 忽略 `capture`/`into`, 把整个 args 解析为 `VoteResult`, 按**投票者 agent id** 存 `votes_[agent]`, **不写文本槽**.

### 3.5 模板变量

`{{topic}}` `{{round}}` `{{max_rounds}}` `{{round_context}}` `{{residual_concerns}}` `{{round_outputs}}` `{{<slot>.current}}` `{{<slot>.prev}}` `{{<var>}}`

- `slot` = `into` 值 (默认 agent id). `current` = 本轮该槽; `prev` = 上一轮该槽 (每轮末快照).
- `{{round_context}}` = 上一轮 `voteSummary()` (本轮投票前为空).
- `{{residual_concerns}}` = `residualConcerns()`.
- `{{round_outputs}}` = 本轮全部 text 捕获, 按 action 顺序拼: `"[" + display_name + "]\n" + text + "\n\n"`.
- 共享上下文 (`{{session_context}}` 已删除): 旧实现是把它当 **system message** 注入每个 agent (seedSharedContext), v1.1 沿用, 不做模板变量.
- 缺失/未定义 -> 空串; `fillTemplate` 永不抛.

### 3.6 tools/*.json

技能级**发言工具**定义 (name/description/parameters). 让每个技能自定义工具名 (bigbang 用 `bigbang_turn/vote/doc`; 别的技能可叫 `speak`/`check`).

---

## 4. 引擎接口

### 4.1 SubAgent (role runtime, `sub_agent.h/.cpp`)

| 成员 | 类型 | 说明 |
|------|------|------|
| `name_` `systemPrompt_` `cfg_` `tools_` | | |
| `toolWhitelist_` | vector\<string\> | 能力工具白名单 |
| `toolDefs_` | vector\<ToolDefinition\> | 发言工具 + 能力工具定义 |
| `client_` / `session_` | DeepSeekClient / Session | |
| `lastReasoning_` / `lastError_` | string | |
| `toolInterceptor_` | function\<string(string,string)\> | request_agent |
| `responder_` | BigbangResponder | test seam |

方法: `configure(cfg,tools,whitelist,maxFileRounds)` / `registerToolDef` / `seedSharedContext` / `setToolInterceptor` / `setResponder` / `turn(message,verb,force)->string` / `cancel()`.

- `requestTool(verb,force,out)`: `doRequest(verb,force)`; 若 `!valid` 且 `lastError_` 含 `"tool_choice"` -> 用 `force=false` 重试一次.
- `doRequest` 内: `toolChoice = force ? verb : "auto"`; **`lastReasoning_` 仅在此清空** (thinking 多轮必需).

### 4.2 `turn()` 精确流程

```
turn(msg, verb, force):
  session_.addMessage(Message::User(msg))
  for r in 0..maxFileRounds_:
    string text; tc = requestTool(verb, force, text)
    if !tc.valid:
       out = !lastError_.empty() ? "[error] " + lastError_
           : !text.empty()      ? text
           :                      ""                       // 无 tool_call 且无文本
       if (!out.empty()) session_.addMessage(Message::Assistant(out))
       return out
    Message am = Message::Assistant(); am.toolCalls={tc}; am.reasoningContent=lastReasoning_
    session_.addMessage(am)
    if tc.name == verb:
       files = args["file_requests"]
       if (!files.empty() && r < maxFileRounds_):
          session_.addMessage(Message::Tool(tc.id, verb, executeFileRequests(files))); continue
       session_.addMessage(Message::Tool(tc.id, verb, "OK")); return tc.arguments   // 原样 args JSON
    else:                                                                            // 侧工具
       session_.addMessage(Message::Tool(tc.id, tc.name, dispatchSide(tc))); continue
  return ""
```

`dispatchSide(tc)`: `request_agent` 且有 interceptor -> 调 interceptor; 否则 `tc.name` 在白名单 -> `tools_->execute`; 否则 `"Error: tool not allowed"`.

**边界行为 (对照旧 `roundtable.cpp`, 标注偏差):**

| 情形 | 旧行为 | v1.1 行为 |
|------|--------|-----------|
| API 出错 | 返回 `"[bigbang error] " + msg` | `"[error] " + msg` (**偏差**, 引擎不含 bigbang 字样) |
| 无 tool_call 但有文本 | 返回该文本 | 同 |
| 无 tool_call 且无文本 | 返回 `"(no statement)"` | 返回 `""` (**偏差**, 由 SkillRunner 不推气泡) |
| 文件请求往返 | 上限 2 轮, 每轮 `Tool(...,"OK"\|文件内容)` | 同 |

> 偏差只出现在**异常/边界路径**, 正常路径 (§8 五类消息) 逐字节一致. 金标对拍用脚本化 responder 走正常路径.

### 4.3 SkillRunner (blackboard 解释器, `skill_runner.{h,cpp}`)

成员: `cfg_ tools_ cbs_ topic_ sharedContext_ skillsDir_ config_` / `agents_: map<id,unique_ptr<SubAgent>>` / `slots_ prevSlots_: map<slot,string>` / `votes_: map<agentId,VoteResult>` / `roundContext_` / `cancelRequested_`.

**并发规则:** `executeSteps` 每个 action 在独立线程里只写**本地槽** `result[i]`; 全部 join 后由 runner 线程**顺序合并**进 `slots_/votes_` 并触发事件. 共享 map 只被 runner 线程触碰 -> 无锁、无竞争.

```
run(topic):
  if !modelSupportsTools(cfg_.model): event(system, msg); return
  load config.json ; load tools/*.json -> map<name,ToolDefinition>
  # 派生式注册 (v1.1): 扫所有 action, 得 perAgent[agent] = {tool...}
  for a in agents:
     verbDefs = { jsonDef[t] : t in perAgent[a.id] }   # 发言工具 (来自 tools/*.json, 按 action 派生)
     capDefs  = { regDef[t]  : t in config_.tools }    # 能力工具 (来自 ToolRegistry 定义)
     defs = verbDefs + capDefs
     ag = SubAgent(a.display_name, read(a.prompt))
     ag.configure(cfg_, tools_, config_.tools, 2); for d in defs: ag.registerToolDef(d)
     ag.seedSharedContext(sharedContext_)
     if dynamic_dispatch: ag.registerToolDef(defs["request_agent"]); ag.setToolInterceptor(...)
     agents_[a.id] = ag
  for round in 1..max_rounds:
     if cancelRequested_: break
     executeSteps(round_steps, round)
     if cancelRequested_: break
     if stopRule(): converged = true; break
     roundContext_ = voteSummary(); prevSlots_ = slots_
  if cancelRequested_: progress("Cancelled"); return
  executeSteps(on_converge, lastRound)          # 出文档
  progress(converged ? "Done (converged)" : "Done (round cap reached)")
```

`executeSteps(steps, round)`: 每 step: `whenMatches` 否则跳过; 有 `progress` -> progress(fill); parallel -> 线程写 `result[i]` 后 join, 否则顺序; join 后逐个 `mergeAction(a, result[i])`. (每 step 前查 cancel.)
`whenMatches(w, round)`: `""`->true; `"round==1"`->round==1; `"round>1"`->round>1.

`mergeAction(a, r)` -- **按配置, 不按工具名** (**Bug A 修复: vote 不写文本槽**):
```
if a.shape == "vote":
   if r.empty():          votes_[a.agent] = {agree=false, concerns=["[high] (no structured vote)"]}
   else if isJson(r):     votes_[a.agent] = parseVote(fromJson(r))
   else:                  votes_[a.agent] = {agree=false, concerns=["[high] "+r]}   // r = "[error] ..."
   if a.emit=="vote": event(display(a.agent), vote, voteLine(votes_[a.agent]))
   return                                              // 不写 slots_ (旧代码 votes_ 与 leonardS_ 本分离)
else: // text
   s = r.empty() ? "" : (a.capture.empty() ? trim(r) : value(fromJson(r), a.capture))
   slots_[a.into] = s
   if (!s.empty()):
      if a.emit=="message":  event(display(a.agent), message, s)
      if a.emit=="document": event(display(a.agent), document, s)
```

`stopRule()` (**Bug B 修复: 只看真正投票的条目**):
```
if loop_detect nonempty && round>1 && !slots_[ld].empty() && slots_[ld]==prevSlots_[ld]: return true
if converge=="all_agree" && !votes_.empty() && all(v.agree for v in votes_.values()): return true
return false
```

`voteSummary()` / `residualConcerns()`: 按 `agents` 配置顺序, 逐 agent 读 `votes_[agent.id]` (无则跳过); 格式严格复刻旧 `buildVoteSummary`/`buildResidualConcerns` (见 §8 字节基准).
`voteLine(vr)`: `(vr.agree?"AGREE":"DISSENT") + (vr.concerns非空 ? " (concerns: "+join("; ")+")" : "")`.

`handleRequestAgent(id,msg,depth)`: `depth>=max_recursion` -> `"[error] max recursion depth exceeded"`; 未知 id -> `"[error] unknown agent"`; 否则 `agents_[id]->turn(msg, firstVerbToolOf(id), false)` 返回原始结果 (`firstVerbToolOf(id)` = 该 agent 在本程序中出现的第一个发言工具; 引擎不含任何工作流专属名).

### 4.4 事件 (统一出口, 严格保真)

`onEvent(role, kind, text)`, `kind ∈ {message, vote, document, system}`. TUI 按 kind 生成气泡 (**必须逐字节复刻旧 `app_tui.cpp`**):

| kind | 气泡文字 | 旧对应 |
|------|---------|--------|
| `message` | `"**[" + role + "]** " + text` | onStatement |
| `vote` | `"**[" + role + " vote]** " + text` (text = voteLine) | onVote |
| `document` | `text` (原样, 无前缀) | onDoc |
| `system` | `"**[" + role + "]** " + text` (role="system") | onStatement("system",...) |

---

## 5. bigbang 兼容 (内置技能 #1)

1. **配置实例:** `bigbang_debate` 是 §8 的一个 JSON; 程序 = 提案/整合/投票, 停止 = `all_agree`, 后程序 = doc.
2. **逐字节基准 (自包含):** §8 的五类 message 与旧 `roundtable.cpp run()` 的 C++ 拼接**逐字节相等**; `voteSummary`/`residualConcerns`/`<<display>>` 前缀见 §4.3/§4.4. 正常路径零偏差; 异常路径偏差见 §4.2 表.
3. **四阶段迁移 (低风险):** 并存(新引擎 + `/skill`, 旧 `/bigbang` 不动) -> 金标对拍(用脚本化 responder 驱动旧 `Roundtable` 记录实发消息, 断言 `fillTemplate(config) == 原串`) -> 切别名(`/bigbang` -> `startSkill("bigbang_debate")`) -> 删旧(roundtable.* + prompts 死码).
4. **回归保护:** 旧 3 个收敛契约(全票收敛 / 轮次上限仍出文档 / loop-detect)在阶段 D 重写为 SkillRunner 测试锁定.

---

## 6. 文件清单与阶段

| 文件 | 操作 | 阶段 |
|------|------|------|
| `src/core/skill_config.h` | 新建 (SkillConfig/Agent/Step/Action + VoteResult) | A |
| `src/core/sub_agent.{h,cpp}` | 新建 (role runtime) | A |
| `src/core/skill_runner.{h,cpp}` | 新建 (解释器 + 模板 + 播种 + 调度) | A |
| `src/tui/app_tui.{h,cpp}` | 改 (`/skill` 路由 + `onEvent` 回调 + `startSkill`) | A/C/D |
| `src/core/roundtable.{h,cpp}` | 删 | D |
| `src/core/prompts_loader.cpp` / `prompts.h` | 删 bigbang 死码 (内容迁入 `ensureDefaultSkills` 播种) | D |
| `src/tui/main_tui.cpp` | `[bigbang]` -> `[skill]` | D |
| `tests/unit/test_skill_{messages,runner}.cpp` | 新建 (金标 + 收敛契约) | B/D |
| `CMakeLists.txt` / `tests/CMakeLists.txt` | 改 | A/D |

引擎侧无新依赖; 复用 `ToolRegistry` / `Session` / `DeepSeekClient`; 不改 IProcessRunner 与 TUI 渲染链路. 技能文件 (config/prompts/tools) 由 `ensureDefaultSkills()` 运行期播种到 `projv_files/skills/`, 免依赖 exe 目录.

---

## 7. 验证清单

| # | 验证项 | 阶段 |
|---|--------|------|
| 1 | 金标: bigbang 五类消息逐字节相等 (正常路径) | B |
| 2 | `/bigbang` UX 不变 (气泡/状态栏/Esc) | C |
| 3 | 三收敛契约 (全票 / 轮次上限出文档 / loop-detect) | D |
| 4 | 白名单生效: 辩论角色不能 `exec_shell`/`write_file` | A |
| 5 | 泛化: 把 bigbang 工具名换成 `speak`, 仅改 config 即等价 | A |
| 6 | `{{round_outputs}}` 汇总全场; `{{slot.current/prev}}` 正确 | A |
| 7 | `converge:none` 跑满轮次执行 `on_converge` | A |
| 8 | 五类工作流各一份 config 均能跑 (圆桌/专家/脑暴/kanban/蜂群) | A |
| 9 | `request_agent` 超 `max_recursion` 返回 error 不崩 | A |
| 10 | Bug A 回归: 投票后 `{{leonard.prev}}` 仍是方案而非投票文本 | D |
| 11 | Bug B 回归: 未投票的 agent 不会让 `all_agree` 永假 | D |
| 12 | 全量构建 + 现有单测通过 | 各阶段 |

> 已接受的偏差 (不计入失败): 异常路径文案 (`[error] ` vs `[bigbang error] `; 空回复 `""` vs `"(no statement)"`), 见 §4.2.

---

## 8. `skills/bigbang_debate/config.json` (内置技能 #1)

> 与旧 `roundtable.cpp` 正常路径逐字节对齐. 字段 `capture/into/shape/emit` 即通用化写法; vote 无 `into` (按 agent 存).

```json
{
  "name": "bigbang_debate",
  "description": "Three-role engineering debate: propose, integrate, vote, emit an execution doc.",
  "tools": ["read_file", "grep_files", "file_search"],
  "agents": [
    {"id": "sheldon", "display_name": "Sheldon", "prompt": "sheldon.md"},
    {"id": "penny",   "display_name": "Penny",   "prompt": "penny.md"},
    {"id": "leonard", "display_name": "Leonard", "prompt": "leonard.md"}
  ],
  "max_rounds": 8,
  "converge": "all_agree",
  "loop_detect": "leonard",
  "round_steps": [
    {"when": "round==1", "parallel": true,
     "progress": "Round {{round}}/{{max_rounds}} - proposals (Sheldon + Penny)",
     "actions": [
       {"agent": "sheldon", "tool": "bigbang_turn", "capture": "statement", "into": "sheldon", "emit": "message",
        "message": "Topic: {{topic}}\n\nCall bigbang_turn with your proposal."},
       {"agent": "penny", "tool": "bigbang_turn", "capture": "statement", "into": "penny", "emit": "message",
        "message": "Topic: {{topic}}\n\nCall bigbang_turn with your proposal."}
     ]},
    {"when": "round>1", "parallel": true,
     "progress": "Round {{round}}/{{max_rounds}} - proposals (Sheldon + Penny)",
     "actions": [
       {"agent": "sheldon", "tool": "bigbang_turn", "capture": "statement", "into": "sheldon", "emit": "message",
        "message": "Topic: {{topic}}\n\n=== Round {{round}} ===\n\nLeonard compromise from previous round:\n{{leonard.prev}}\n\nPrevious vote results:\n{{round_context}}\n\nPenny's previous proposal:\n{{penny.prev}}\n\nCall bigbang_turn."},
       {"agent": "penny", "tool": "bigbang_turn", "capture": "statement", "into": "penny", "emit": "message",
        "message": "Topic: {{topic}}\n\n=== Round {{round}} ===\n\nLeonard compromise from previous round:\n{{leonard.prev}}\n\nPrevious vote results:\n{{round_context}}\n\nSheldon's previous proposal:\n{{sheldon.prev}}\n\nCall bigbang_turn."}
     ]},
    {"when": "round==1", "progress": "Round {{round}}/{{max_rounds}} - integration (Leonard)",
     "actions": [
       {"agent": "leonard", "tool": "bigbang_turn", "capture": "statement", "into": "leonard", "emit": "message",
        "message": "Topic: {{topic}}\n\n=== Round {{round}} Integrate ===\n\nSheldon:\n{{sheldon.current}}\n\nPenny:\n{{penny.current}}\n\nCall bigbang_turn."}
     ]},
    {"when": "round>1", "progress": "Round {{round}}/{{max_rounds}} - integration (Leonard)",
     "actions": [
       {"agent": "leonard", "tool": "bigbang_turn", "capture": "statement", "into": "leonard", "emit": "message",
        "message": "Topic: {{topic}}\n\n=== Round {{round}} Integrate ===\n\nSheldon:\n{{sheldon.current}}\n\nPenny:\n{{penny.current}}\n\nPrevious vote results:\n{{round_context}}\n\nCall bigbang_turn."}
     ]},
    {"parallel": true, "progress": "Round {{round}}/{{max_rounds}} - vote",
     "actions": [
       {"agent": "sheldon", "tool": "bigbang_vote", "shape": "vote", "emit": "vote",
        "message": "Topic: {{topic}}\n\n=== Round {{round}} Vote ===\n\nProposal under vote:\n{{leonard.current}}\n\nFor reference - Sheldon position:\n{{sheldon.current}}\n\nFor reference - Penny position:\n{{penny.current}}\n\nCall bigbang_vote."},
       {"agent": "penny", "tool": "bigbang_vote", "shape": "vote", "emit": "vote",
        "message": "Topic: {{topic}}\n\n=== Round {{round}} Vote ===\n\nProposal under vote:\n{{leonard.current}}\n\nFor reference - Sheldon position:\n{{sheldon.current}}\n\nFor reference - Penny position:\n{{penny.current}}\n\nCall bigbang_vote."},
       {"agent": "leonard", "tool": "bigbang_vote", "shape": "vote", "emit": "vote",
        "message": "Topic: {{topic}}\n\n=== Round {{round}} Vote ===\n\nProposal under vote:\n{{leonard.current}}\n\nFor reference - Sheldon position:\n{{sheldon.current}}\n\nFor reference - Penny position:\n{{penny.current}}\n\nCall bigbang_vote."}
     ]}
  ],
  "on_converge": [
    {"progress": "Writing execution document (Leonard)",
     "actions": [
       {"agent": "leonard", "tool": "write_bigbang_doc", "capture": "doc_markdown", "emit": "document",
        "message": "Topic: {{topic}}\n\nFinal approved proposal:\n{{leonard.current}}\n\nResidual concerns:\n{{residual_concerns}}\n\nCall write_bigbang_doc."}
     ]}
  ]
}
```

配套 `tools/*.json` (发言工具定义, 由 `ensureDefaultSkills` 播种):

```text
bigbang_turn.json : {"name":"bigbang_turn","parameters":[{"name":"statement","type":"string","required":true},{"name":"file_requests","type":"array","required":false}]}
bigbang_vote.json : {"name":"bigbang_vote","parameters":[{"name":"agree","type":"boolean","required":true},{"name":"agreed_points","type":"array","required":false},{"name":"concerns","type":"array","required":false},{"name":"suggested_tweak","type":"string","required":true}]}
write_bigbang_doc.json : {"name":"write_bigbang_doc","parameters":[{"name":"doc_markdown","type":"string","required":true}]}
```

---

## 9. 同一引擎覆盖其它工作流 (仅配置)

**专家会议** (1 轮, 主持汇总):
```json
{ "name": "expert_panel", "max_rounds": 1, "converge": "none",
  "agents": [
    {"id": "e1", "display_name": "Expert A", "prompt": "e1.md"},
    {"id": "e2", "display_name": "Expert B", "prompt": "e2.md"},
    {"id": "host", "display_name": "Host", "prompt": "host.md"}
  ],
  "round_steps": [
    {"parallel": true, "actions": [
      {"agent": "e1", "tool": "speak", "capture": "text", "into": "e1", "emit": "message",
       "message": "Topic: {{topic}}\nGive your expert view."},
      {"agent": "e2", "tool": "speak", "capture": "text", "into": "e2", "emit": "message", "message": "Topic: {{topic}}."}
    ]},
    {"actions": [
      {"agent": "host", "tool": "speak", "capture": "text", "into": "summary", "emit": "document",
       "message": "Topic: {{topic}}\n\nAll views:\n{{round_outputs}}\n\nSynthesize."}
    ]}
  ]}
```

**头脑风暴** (N 发散 -> 聚类): 与专家会议同构, `max_rounds:1`, 末步 `synthesizer` 读 `{{round_outputs}}` 聚类.

**Kanban** (多轮=重计划; 每 worker 不同 `into`; 复核投票):
```json
{ "name": "kanban", "max_rounds": 4, "converge": "all_agree", "loop_detect": "review",
  "agents": [
    {"id": "planner", "display_name": "Planner", "prompt": "planner.md"},
    {"id": "w1", "display_name": "Worker 1", "prompt": "worker.md"},
    {"id": "w2", "display_name": "Worker 2", "prompt": "worker.md"},
    {"id": "w3", "display_name": "Worker 3", "prompt": "worker.md"},
    {"id": "review", "display_name": "Reviewer", "prompt": "review.md"}
  ],
  "round_steps": [
    {"actions": [
      {"agent": "planner", "tool": "speak", "capture": "text", "into": "plan", "emit": "message",
       "message": "Topic: {{topic}}\nPlan the tasks."}
    ]},
    {"parallel": true, "actions": [
      {"agent": "w1", "tool": "speak", "capture": "text", "into": "task1", "emit": "message", "vars": {"task": "task 1"},
       "message": "Do {{task}} per plan:\n{{plan.current}}"},
      {"agent": "w2", "tool": "speak", "capture": "text", "into": "task2", "emit": "message", "vars": {"task": "task 2"},
       "message": "Do {{task}} per plan:\n{{plan.current}}"},
      {"agent": "w3", "tool": "speak", "capture": "text", "into": "task3", "emit": "message", "vars": {"task": "task 3"},
       "message": "Do {{task}} per plan:\n{{plan.current}}"}
    ]},
    {"actions": [
      {"agent": "review", "tool": "check", "shape": "vote", "emit": "vote",
       "message": "Plan:\n{{plan.current}}\n\nResults:\n{{round_outputs}}\n\nApprove?"}
    ]}
  ]}
```

**蜂群** (有限): `dynamic_dispatch:true`, `max_recursion:3`, `converge:"none"`, `round_steps` 单步让 manager 用 `request_agent` 调度.

> 关键结论: 从圆桌到 kanban, **引擎零改动, 只换 config**. 工具名 (`speak`/`check`) 由各技能自己的 `tools/*.json` 定义, 引擎不预置任何工作流专属语义.
