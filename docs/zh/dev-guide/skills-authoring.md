# 编写 skill —— 不看源码也能搭出 sub-agent 编排

> 读者：需要产出一个**能跑**的 skill 目录的人或 agent。看完这一页就够，不需要读 C++。
> 姊妹页：[模块地图](module-map.md)（引擎在哪）、[常见做法](recipes.md)。

---

## 0. 六十秒理解

1. **skill 是一个文件夹**，不是代码：`config.json` + 每个 agent 一个 `.md` 提示词 + `tools/*.json`。丢进 `projv_files/skills/` 就能跑，不需要编译。
2. 引擎是个解释器：跑**轮**（round）→ 每轮跑**步骤**（step）→ 每步跑**动作**（action）→ 每个动作 = 一个 agent + 一个 verb 工具 + 一条消息。
3. 两种编排模式：**剧本模式**（引擎掌握阵容与顺序）或**现场模式**（只有一个 *driver* 决定下一步派谁）。二选一，同一个 skill 里不能混。
4. 两条硬规则决定产出有没有东西：**引擎只发聊天事件**（文件必须由 agent 自己写）；**模板写错不报错**（未知 `{{key}}` 渲染成空串）。
5. 安装 = 复制文件夹。调用 = `/skill <名字> <题目>`、`/<名字> <题目>`，或主 agent 的 `run_skill`。

---

## 1. 目录结构与安装

```
myskill/
├── config.json          # 程序：agents、步骤、停止规则
├── lead.md              # 每个 agent id 一个系统提示词
├── critic.md
└── tools/
    └── emit.json        # 每个 verb 工具一个文件
```

| 项 | 规则 |
|---|---|
| 文件夹名 | 就是 skill 名和斜杠命令。ASCII、小写、无空格、≤30 字符 |
| 提示词文件 | 由 `agents[].prompt` 指定，相对 skill 文件夹 |
| verb 工具 | `tools/` 下每个 `*.json`；文件名随意，**`name` 字段才是关键** |
| 安装 | 复制到 `<exeDir>/projv_files/skills/<文件夹>`（Windows 就是 `proJV_tui.exe` 旁边） |
| 发现 | `/skill` 或 `/skill list` |
| 运行 | `/skill myskill <题目>`、`/myskill <题目>`，或主 agent 的 `run_skill` |
| 停止 | `Esc` 在下一个动作/步骤/轮边界取消 |

能力工具（`read_file`、`exec_shell`…）来自应用注册表，**全 skill 共享**：每个 agent 看到同一份清单。verb 工具是 skill 私有的，但也发给每个 agent——所以一个在程序里没有动作的 worker 仍然有工具可回话。

---

## 2. 选模式

| 问题 | 剧本模式 | 现场模式 |
|---|---|---|
| 阵容在开跑前就确定？ | 是 | 否 |
| 必须所有人同意？ | 是：`converge=all_agree` | 否 |
| 由一个 agent 决定下一步派谁？ | 否 | 是：一个 **driver** 用 `request_agent` |
| 谁驱动 | `round_steps` | driver 自己的回合 |
| 收敛判据 | 机械数票，或 `stop_when` | driver 写的 slot 满足 `stop_when` |
| 例子 | `steve`、`bigbang` | `avenger`、`musk`、`monica` |

**决定性的那条**：阵容固定且必须全票通过 → **剧本模式**，让引擎数票。阵容必须在运行中挑选 → **现场模式**，且只有一个 driver。两者不可兼得：driver 不能作为被派发对象，所以把派发者放进投票步骤会让整轮报错（`cannot dispatch to a driver agent`）。

---

## 3. `config.json` 顶层

| 键 | 类型 | 默认 | 允许值 / 范围 | 含义 |
|---|---|---|---|---|
| `name` | string | -（**必填**） | 与文件夹同名 | skill 名 = 斜杠命令 |
| `description` | string | `""` | 自由文本 | 一行说明，`/skill list` 显示 |
| `tools` | string[] | `[]` | 注册表里的工具名 | 所有 agent 共享的能力白名单 |
| `agents` | object[] | -（**必填**） | 见下 | 阵容 |
| `max_rounds` | int | `8` | ≥1 | 轮数硬上限 |
| `converge` | string | `"none"` | `none` \| `all_agree` | 全票同意即停 |
| `loop_detect` | string | `""` | 一个 slot 名 | 该 slot 两轮不变即停 |
| `dynamic_dispatch` | bool | `false` | - | 现场模式：driver 获得 `request_agent` |
| `dispatch_tool` | string | `""` | 一个 verb 工具名 | 被派发 worker 用的 verb（`""` = 该 worker 的第一个工具） |
| `max_tool_iters` | int | `3` | ≥1 | 每个 agent 回合的工具调用预算 |
| `submit_reserve` | int | `0` | `0 .. max_tool_iters-1` | 预留最后 N 次给提交工具 |
| `max_parallel` | int | `16` | `1 .. 64` | 一个回合内并发派发的组数 |
| `tool_mode` | string | `"verb"` | `verb` \| `auto` | `verb` 锁定动作工具；`auto` 让模型自选 |
| `thinking` | string | `"on"` | `on` \| `off` | 整个 skill 的思考模式 |
| `stop_when` | object | - | `{slot, equals}` | `slots[slot] == equals` 时停止轮循环 |
| `round_steps` | step[] | `[]` | 见 §4 | 程序主体 |
| `on_converge` | step[] | `[]` | 见 §4 | 循环结束后跑一次 |

**`agents[]`**

| 字段 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `id` | string | -（**必填**） | 动作里引用的名字；小写加 `_` |
| `display_name` | string | `id` | 聊天区与侧栏显示为 `[名字]` |
| `prompt` | string | `""` | skill 文件夹内的文件名 |
| `thinking` | string | `""` | `""` = 继承 skill 级；`on`/`off` 覆盖 |

**加载时直接拒绝**（整个运行停下并报错）：`tool_mode`、`converge`、`thinking`、`when`、`shape`、`emit` 出现未知值；`max_rounds < 1`；`max_tool_iters < 1`；`submit_reserve >= max_tool_iters`；`max_parallel` 不在 1..64；动作引用了未声明的 agent；agent 缺 `id`；缺 `name`。

**只警告不拒绝**（聊天里出现 `[skill] config: unknown key 'x'`，并给"你是不是想写"的编辑距离建议）：加载器不认识的任何键。**把它当错别字处理，不要当噪音。**

---

## 4. 步骤与动作

**步骤（step）**

| 字段 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `when` | string | `""` | `""`（总是）\| `round==1` \| `round>1` |
| `parallel` | bool | `false` | 本步所有动作并发执行 |
| `progress` | string | `""` | 执行期间状态行显示的模板 |
| `actions` | object[] | `[]` | 见下 |

**动作（action）**

| 字段 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `agent` | string | -（**必填**） | 必须是已声明 id |
| `tool` | string | `""` | agent 必须调用的 verb；`tool_mode=auto` 时 `""` 表示让模型自选 |
| `message` | string | `""` | 本回合指令，是模板（§5） |
| `capture` | string | `""` | 从回复 JSON 里取哪个字段（`shape=text`） |
| `into` | string | agent id | 结果写入哪个 slot |
| `shape` | string | `"text"` | `text` \| `vote` |
| `emit` | string | `""` | `""` \| `message` \| `vote` \| `document` —— 聊天区显示什么 |
| `vars` | object | `{}` | 仅本动作消息内可用的 `{{名字}}` |

**回复怎么合入**

| shape | 回复 | 结果 |
|---|---|---|
| `text` | 含 `capture` 字段的 JSON | 该字符串进 slot |
| `text` | 其它任何内容 | 原文进 slot |
| `vote` | `{"agree":bool,"agreed_points":[],"concerns":[],"suggested_tweak":"..."}` | 记为一张票 |
| `vote` | 非 JSON / 空 / 错误串 | 记为**反对票**并附 `[high]` concern——所以一次失败的调用会故意阻断 `all_agree` |
| 任意 | `emit: "document"` | 该文本同时成为本次运行的最终文档 |

---

## 5. 模板 —— 以及那个坑

`message` 与 `progress` 里可用：

| 变量 | 值 |
|---|---|
| `{{topic}}` | 启动 skill 时的题目 |
| `{{round}}` / `{{max_rounds}}` | 当前轮 / 上限 |
| `{{round_context}}` | 上一轮的投票摘要 |
| `{{residual_concerns}}` | 每条异议一行 |
| `{{round_outputs}}` | 本轮所有 `emit: message` 的文本，带角色前缀 |
| `{{skills_dir}}` | skills 目录绝对路径 |
| `{{X.current}}` | 本轮写入的 slot `X` |
| `{{X.prev}}` | **上一轮结束时**的 slot `X` |
| `{{变量名}}` | 动作 `vars` 里的值 |

**按咬人顺序排列的五个坑**

1. **未知 key 渲染成空串且不报警。** `{{topic.current}}` 不是 slot，静默变成空。agent 表现得像"没被告知任何东西"时，先查花括号。
2. **没有 `slot.` 前缀。** 是 `{{design_pm.current}}`，不是 `{{slot.design_pm.current}}`。
3. **`.prev` 是上一轮结束时的快照。** 循环停止的那一轮不会更新它——所以"你上一轮的判断"必须用 `.prev`，不能用 `.current`。
4. **`vars` 会遮蔽同名 slot**（仅对声明它的那个动作）。
5. 模板只是字符串替换：没有条件、没有循环。分支靠 `when` 和拆步骤表达。

---

## 6. 执行模型

```
for round = 1 .. max_rounds:
    对 round_steps 里每个满足 when 的 step:
        若 parallel: 所有动作并发，结束后统一合入
        否则: 按顺序执行，逐个合入
    若 stopRule(round): converged = true; break
    round_context = 投票摘要; prev = slots 快照
未取消则执行 on_converge（round = 最后到达的轮）
```

**停止规则，每轮结束后按此顺序检查**

1. `stop_when`：`slots[slot] == equals`（**精确字符串**匹配，所以要用 `DONE` 这类 token）。
2. `loop_detect`：指定 slot 非空且与上一轮完全相同（第 1 轮不生效）。
3. `converge: all_agree`：至少有一张票，且每张票都是 `agree: true`。

**值得知道的几件事**

- 无论收敛还是撞到轮数上限，`on_converge` 都会跑——**除非被取消**，取消则跳过。
- `parallel: true` 但只有一个动作时，等价于串行。
- 并行步骤里的动作互不相干：所有动作结束后按声明顺序合入 slot。
- 取消在动作间、步骤间、轮间检查；进行中的 API 调用由取消探针中止。

---

## 7. 预算

| 旋钮 | 它替你防什么 |
|---|---|
| `max_tool_iters` | 永远不提交的死循环。短回合 3，调研 40+，长实现 120。 |
| `submit_reserve` | 预留最后 N 次给 verb：快超预算时会提示 agent 停止探索、去提交。必须小于 `max_tool_iters`。 |
| `max_parallel` | driver 一次回复里派发太多导致的并发失控。 |
| `tool_mode: verb` | 模型用散文回答而不调 verb。它会锁定工具，回复就是该工具的参数 JSON。 |
| `tool_mode: auto` | 动作需要用能力工具（`read_file`、`web_search`）时必须开。`dynamic_dispatch` 强制为 `auto`。 |
| `thinking` | 成本与延迟。真正有用的是按 agent 覆盖：编排者开，廉价 worker 关。 |

---

## 8. driver 规则（以及它的代价）

- **driver = 在任何动作里出现过的 agent**（`round_steps` 或 `on_converge` 里都算）。
- 只有 driver 能拿到 `request_agent`，且必须 `dynamic_dispatch: true`。
- **driver 永远不能作为被派发对象**（`cannot dispatch to a driver agent`）。
- worker 是叶子：没有 `request_agent`，也不能跟另一个 worker 说话。**driver 是唯一通道**——两个 worker 要交换信息，必须由 driver 转述。
- 派发只有一层深，设计如此，没有 `dispatch_from`。

---

## 9. 交付纪律 —— 每个 skill 都会踩的那一脚

引擎只发聊天事件，**从不替你写文件**。一个收尾只"汇报"的 skill，会产出一段漂亮对话和磁盘上的一片空白。

在收尾动作里明确要求：

1. `md_file` `action=write` 写整个文档；
2. 超过 8 KB 就用 `action=append` 续写；
3. `read_file` 读回文件尾部，证明没被截断；
4. 这时才调用向聊天区发文档的那个 verb。

同时定好落盘位置（本仓库的惯例是 `docs/sop/<name>-<slug>.md`），并要求**开头先写一页摘要**——不翻附录的读者也能拿到答案。

---

## 10. 例 A —— 剧本模式 + 全票通过

三个 agent，一次辩论，机械停止，最后落盘。

```json
{
  "name": "debate",
  "description": "Draft a plan, critique it from two stances, vote until unanimous, write the plan.",
  "thinking": "on",
  "tools": ["read_file", "grep_files", "md_file"],
  "tool_mode": "auto",
  "max_tool_iters": 20,
  "submit_reserve": 2,
  "max_parallel": 2,
  "max_rounds": 4,
  "converge": "all_agree",
  "agents": [
    {"id": "lead",    "display_name": "Lead",     "prompt": "lead.md",   "thinking": "on"},
    {"id": "critic_a","display_name": "Critic A", "prompt": "critic.md", "thinking": "off"},
    {"id": "critic_b","display_name": "Critic B", "prompt": "critic.md", "thinking": "off"}
  ],
  "round_steps": [
    {"when": "round==1", "progress": "Round {{round}}/{{max_rounds}} - lead drafts",
     "actions": [
       {"agent": "lead", "tool": "draft", "capture": "text", "into": "plan", "emit": "message",
        "message": "Topic: {{topic}}\n\nWrite the first version of the plan, at most 40 lines. Call draft(text=...)."}
     ]},
    {"progress": "Round {{round}}/{{max_rounds}} - lead revises",
     "actions": [
       {"agent": "lead", "tool": "draft", "capture": "text", "into": "plan", "emit": "message",
        "message": "Plan on the table:\n{{plan.current}}\n\nDissent from the last vote:\n{{residual_concerns}}\n\nRevise the FULL plan (not a diff), at most 40 lines. Call draft(text=...)."}
     ]},
    {"parallel": true, "progress": "Round {{round}}/{{max_rounds}} - vote",
     "actions": [
       {"agent": "critic_a", "tool": "vote", "shape": "vote", "emit": "vote",
        "message": "Plan:\n{{plan.current}}\n\nVote as the risk stance. A [high] concern forbids agree. Call vote."},
       {"agent": "critic_b", "tool": "vote", "shape": "vote", "emit": "vote",
        "message": "Plan:\n{{plan.current}}\n\nVote as the cost stance. A [high] concern forbids agree. Call vote."}
     ]}
  ],
  "on_converge": [
    {"progress": "Writing the plan",
     "actions": [
       {"agent": "lead", "tool": "write_plan", "capture": "doc_markdown", "emit": "document",
        "message": "FIRST persist the document: md_file action=write path=docs/sop/debate-<slug>.md, append chunks under 8KB, read the tail back. THEN call write_plan with the same Markdown.\n\nAgreed plan:\n{{plan.current}}\n\nResidual concerns:\n{{residual_concerns}}"}
     ]}
  ]
}
```

两个提示词文件都写同样的三段：① 你是谁、你守哪一个立场；② 好答案包含什么、限多少行；③ 必须调用哪个 verb，**只用散文回答算失败**。

`tools/draft.json`：

```json
{"name": "draft", "description": "Submit the plan text.",
 "parameters": [{"name": "text", "type": "string", "description": "the full plan", "required": true}]}
```

`tools/vote.json` —— 引擎只认这几个键：

```json
{"name": "vote", "description": "Structured vote on the plan.",
 "parameters": [
   {"name": "agree", "type": "boolean", "description": "true only if no [high] concern remains", "required": true},
   {"name": "concerns", "type": "array", "description": "each '[severity] issue'", "required": false},
   {"name": "suggested_tweak", "type": "string", "description": "one line, or 'none'", "required": false}
 ]}
```

`on_converge` 要么给一个 `write_plan` verb，要么复用 `draft` 配 `capture: "text"`。

---

## 11. 例 B —— 现场模式 + driver

driver 读方案、拆任务、按证据裁决，写出 token 就停。

```json
{
  "name": "crew",
  "description": "One lead reads the plan, dispatches workers, judges on evidence, stops on DONE.",
  "thinking": "on",
  "tools": ["read_file", "write_file", "edit_file", "grep_files", "exec_shell"],
  "tool_mode": "auto",
  "dynamic_dispatch": true,
  "dispatch_tool": "report",
  "max_tool_iters": 120,
  "submit_reserve": 2,
  "max_parallel": 4,
  "max_rounds": 10,
  "converge": "none",
  "stop_when": {"slot": "board", "equals": "DONE"},
  "agents": [
    {"id": "lead",    "display_name": "Lead",     "prompt": "lead.md",   "thinking": "on"},
    {"id": "worker_1","display_name": "Worker 1", "prompt": "worker.md", "thinking": "off"},
    {"id": "worker_2","display_name": "Worker 2", "prompt": "worker.md", "thinking": "off"}
  ],
  "round_steps": [
    {"progress": "Round {{round}}/{{max_rounds}} - lead: dispatch and judge",
     "actions": [
       {"agent": "lead", "tool": "emit", "capture": "summary", "into": "board", "emit": "message",
        "message": "Job: {{topic}}\n\nYour board line from the last round:\n{{board.prev}}\n\nRound 1: read the plan, split it into tasks with a one-line definition of done each, then dispatch with request_agent in ONE reply. Round 2+: judge what came back on evidence and re-dispatch what failed. When every task is done emit exactly DONE, otherwise emit one line."}
     ]}
  ],
  "on_converge": [
    {"actions": [
      {"agent": "lead", "tool": "write_record", "capture": "doc_markdown", "emit": "document",
       "message": "Write the final record as Markdown, persist it with md_file first (append chunks), then call write_record. Job: {{topic}}"}
    ]}
  ]
}
```

lead 的提示词里必须原话写上：**如果你用叙述结束、既没调 `request_agent` 也没调 `emit`，这一轮就算失败**——因为 `dynamic_dispatch` 会把 `tool_mode` 强制成 `auto`，没有任何东西替它锁定工具。

---

## 12. verb 工具文件

```json
{"name": "emit",
 "description": "Report one line of state to the board.",
 "parameters": [
   {"name": "summary", "type": "string", "description": "one line, or exactly DONE", "required": true}
 ]}
```

- `name` 必须与动作里的 `tool` 完全一致。
- `parameters` 是平铺列表：`name`、`type`（`string` | `boolean` | `array` | `integer` | `number`）、`description`、`required`。
- 描述写成命令式短句：模型每一轮都会读到它。

---

## 13. 上线前验证

| 步骤 | 怎么做 | 通过标准 |
|---|---|---|
| 能加载 | `/skill list` 能看到，然后随便给个题目跑一次 | 聊天里没有 `[skill]` 错误行 |
| 无错字 | 看开跑头几秒 | 没有 `[skill] config: unknown key` 警告 |
| 提示词存在 | 每个 `agents[].prompt` 文件都在文件夹里 | 不会以空提示词启动 |
| verb 存在 | 动作里用到的每个 `tool` 都在 `tools/*.json` 的 `name` 里 | agent 第一回合就调得动 |
| 行为正确 | `tests/unit/test_skill_<名字>.cpp`，用 `setResponder` / `setResponderMulti` / `setResponderText`，不联网、不需要 Key | 断言动作顺序、数票、停止规则 |
| 已注册 | 把测试文件加进 `tests/CMakeLists.txt` 的 `TEST_SOURCES`（**不是 glob**） | `ctest` 会跑它 |
| 成本 | `projv_files/logs/skill_metrics.log`：每回合一行（agent、verb、迭代数、结果字节、`report`/`NO_REPORT`） | 迭代数远低于 `max_tool_iters`；没有 `NO_REPORT` |

---

## 14. 起飞前清单

- [ ] `name` 与文件夹同名；ASCII、无空格。
- [ ] 每个 agent 都有 `id` 和存在的 `prompt` 文件。
- [ ] 每个 `action.agent` 都已声明；每个 `action.tool` 都在 `tools/` 里。
- [ ] `submit_reserve` < `max_tool_iters`；`max_parallel` 在 1..64。
- [ ] `when` 只用 `""`、`round==1`、`round>1`。
- [ ] 每条消息里的每个 `{{...}}` 都指向真实 slot、内置变量或 `vars`。
- [ ] 收尾动作要求用 `md_file` 写盘，并读回文件尾自证。
- [ ] `stop_when` 的值是一个让 driver 原样输出的 token（`DONE`）。
- [ ] 开了 `dynamic_dispatch` 时，driver 提示词里写明"最后一定要调 verb"。
- [ ] 有一个脚本化单测，且已注册到 `tests/CMakeLists.txt`。
- [ ] 用一个不重要的题目跑过一次，侧栏 agent 的亮灭符合预期。

---

## 15. 反模式

| 反模式 | 为什么失败 |
|---|---|
| 有"汇报"步骤但没有 `emit` | 聊天区什么也不显示，看起来像静默失败 |
| 指望引擎替你保存文档 | `on_converge` 只发事件；只有 agent 能写文件 |
| 一个大提示词干完所有事 | 没有分回合的预算控制，也没有可观测的进度 |
| 把 driver 放进投票步骤 | 它随即不能作为被派发对象，`all_agree` 永远触发不了 |
| 让 worker 之间交换信息 | worker 是叶子；只有 driver 是通道 |
| 写 `{{slot.x}}` | 不存在 `slot.` 前缀，静默渲染成空 |
| 收尾步骤用 `{{X.current}}` 取"上一轮结论" | 收敛那一轮不更新 `.prev`；应该用 `.prev` |
| 对从不投票的 agent 设 `converge: all_agree` | `votes_` 为空，规则永不触发，一路跑到 `max_rounds` |
| 必须提交的动作却开了 `tool_mode: auto` | 没有东西锁定 verb；要么在提示词里硬要求，要么关掉 `auto` |
| 一次 `write_file` 写超过 8 KB | 工具上限 8 KB；用 `md_file` 的 `append` 分块 |

---

## 16. 已知限制（2026-10）

1. **引擎只发事件。** 除非 agent 自己写，否则什么都没落盘。
2. **skill 内部没有审批门。** 主 agent 的删除审批对话框管不到子代理的 `exec_shell`；只能在提示词里硬约束（只跑构建/测试、只在项目目录内、不 push、不删目录）。
3. **动作失败会被当成异议。** 异常或空回复在投票里变成 `[high]`，所以一次瞬时 API 错误就可能阻断收敛。运行长期不收敛时先看 `skill_metrics.log`。
4. **模板写错是静默的**（§5）。
5. **派发只有一层深**；driver 不能被派发。
6. **撞到 `max_rounds` 不算错误**——进度行会写 `Done (round cap reached)`，`on_converge` 照跑，所以坏掉的停止规则看起来像"慢成功"。
7. **同一时间只有一个子进程。** `exec_shell` 在整个应用内串行（这是设计），并行 worker 无法同时跑 shell。
8. **模板没有条件与循环语法。** 分支用额外步骤 + `when` 表达。
