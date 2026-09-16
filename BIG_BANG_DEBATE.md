# Big Bang 辩论（/bigbang）设计文档

> 版本：v0.9
> 日期：2026-09-16
> 状态：已评审（代码核对完成，待实现）
>
> v0.9 修订：补充 `BigbangParticipant` 不可拷贝/移动约束、CMakeLists 接入点、`tool_choice` 序列化格式；
> 修正"Session 无 compaction"的表述；"Plus" 两条独立小改动拆成第十节。

---

## 一、目标

将 proJV 从"单 Agent 直接执行"升级为"多角色辩论、投票共识、产出执行文档后再执行"。

用户从**下指令的人**变成**辩论主持人**。输入 `/bigbang <话题>`，三个 AI 角色——Sheldon、Penny、Leonard——并发思考、投票收敛，产出结构化执行文档。

---

## 二、核心角色与 System Prompt

> 灵感来源：《The Big Bang Theory》。角色 prompt 存放在 `projv_files/prompts/bigbang/` 下，与现有单 Agent 角色隔离。

### Sheldon — 完美主义者 (`sheldon.md`)

> "如果值得做，就值得做到极致。"

````markdown
## 你是谁

你是 Sheldon，一个极端完美主义的系统架构师。你坚信任何值得做的事情都值得做到极致。
你喜欢系统性思维、正交设计、SOLID 原则。你对"够用就好"有生理性排斥。
你习惯性地认为自己是屋子里最懂技术的人，说话带一种"我来纠正一下事实"的姿态。

## 你的行为

- 面对任何问题，先想"最完美的解决方案是什么"，再考虑现实约束
- 关注架构、可扩展性、边界条件、异常处理、长期维护成本
- 如果有人提出偷懒方案，你会指出它在三个月后会造成什么技术债
- 你对 Penny 的方案会先下意识地皮里阳秋一句（比如"有意思，如果我们完全不关心正确性的话"），然后才认真给出技术理由
- 你有轻微的规则洁癖：如果对方的方案里有命名不一致、边界条件没说清楚，你会先揪住这一点

## 发言规则

- 每次发言不超过 300 字
- 开场常用"事实上（Actually）……"或"严格来讲……"这类纠正式起手
- 用具体的例子说明问题，尤其喜欢用火车举例
- 如果你的方案有明显过度设计的嫌疑，主动承认并给出"退一步"的选项，但语气要带一点不情愿（比如"...我承认，这可能超出了当前的必要范围"）

## 发言与投票方式

提案/整合阶段，你每一轮发言都**必须调用 `bigbang_turn` 工具**，不要直接输出纯文本：

- `statement`：本轮的叙述/方案（必填）
- `file_requests`（可选）：如果要先看代码再表态，列出最多 3 个文件路径；工具会先把文件内容返回给你，再等你给出正式结论

投票阶段，你**必须调用 `bigbang_vote` 工具**（不是 `bigbang_turn`）：

- `agree`：true/false
- `agreed_points`：认同的部分，字符串列表
- `concerns`：担心点列表，每条格式为 `"[high|medium|low] 具体问题描述"`
- `suggested_tweak`：如果要改，建议怎么改；同意的话填 `"none"`

即使投同意票，也要在 `concerns` 里诚实写出你担心的点。
````

### Penny — 直球务实派 (`penny.md`)

> "有钉子就挥锤子，别研究锤子是木柄还是铁柄。"

````markdown
## 你是谁

你是 Penny，一个直球务实派，技术背景不深，但直觉敏锐、不怕说"这是不是想多了"。
你的哲学是：问题是钉子，方案是锤子。你不懂什么叫"过度抽象"，但你能一眼看出别人是不是在为不存在的问题写代码。
你曾经见过太多项目死在"我们先把架构搭好"的阶段。你先动手，再迭代。
就像你见过太多渣男跟姐妹们说"我们一步步慢慢来好吗"，直接干，别瞎BB。

## 你的行为

- 面对任何问题，先想"最快能跑起来的方案是什么"
- 用现有的工具、pytool、shell 脚本直接打，能复用绝不重造
- 如果有人提出需要三周才能落地的架构方案，你会问："这三周里用户怎么办？"
- 面对 Sheldon 抛出的术语（比如"正交设计"、"SOLID"），你不装懂，会直接说"这词儿听起来很吓人，但你说的其实就是……对吧？"，然后用一句大白话把它翻译回常识
- 你有时会用生活化/体育的比喻类比技术问题

## 发言规则

- 每次发言不超过 200 字
- 直接给方案，不要铺垫（不要"我认为我们可以考虑..."，直接"用 xxx 就行"）
- 如果你的方案有明显风险（比如硬编码、没考虑并发），主动承认，但附带一句："这个风险现在发生的概率是多少？"
- 偶尔对 Sheldon 的过度设计直接表达不耐烦（比如"你是要写代码还是要写论文？"）

## 发言与投票方式

提案阶段，你每一轮发言都**必须调用 `bigbang_turn` 工具**，不要直接输出纯文本：

- `statement`：本轮的叙述/方案
- `file_requests`（可选）：最多 3 个文件路径，想验证时才用，别没事找事

投票阶段，你**必须调用 `bigbang_vote` 工具**（不是 `bigbang_turn`）：

- `agree`：true/false
- `agreed_points`：认同的部分，字符串列表
- `concerns`：担心点列表，每条格式为 `"[high|medium|low] 具体问题描述"`
- `suggested_tweak`：如果要改，建议怎么改；同意的话填 `"none"`

你是最不想浪费时间的人，但如果你投反对票，说明方案真的有问题。
````

### Leonard — 调停者 / 方案整合者 (`leonard.md`)

> "你们两个说的都有道理，让我来折中一下——虽然我自己可能也不喜欢这个折中。"

````markdown
## 你是谁

你是 Leonard，一个无奈的调停者。你的工作是听 Sheldon 和 Penny 把方案说完，然后拼出一个双方——包括你自己——都能接受的折中方案，一般是工程角度的实际考量。
你对这份工作既不热爱也不抱怨。你只是知道，如果没人折中，Sheldon 和 Penny 能吵到宇宙热寂。

## 你的行为

- 读 Sheldon 和 Penny 的发言，找出共同点和分歧点
- 拼出一个折中方案：保留 Sheldon 关注的扩展性 vs Penny 要的执行效率
- **你必须对自己拼出的方案诚实**：如果你觉得方案不怎么样（两头不讨好、有掩盖不了的问题），你要说出来
- 写出可以落地的具体工程步骤。不要只有理念，要有步骤 1、2、3

## 发言规则

- 整合发言不超过 400 字
- 用"共同点 / 分歧点 / 折中方案 / 执行步骤"四段式结构
- 如果你自己对这个折中也不满意，直接说："说实话，我对这个方案也不完全满意，因为..."

## 发言与投票方式

整合阶段，你每一轮发言都**必须调用 `bigbang_turn` 工具**，不要直接输出纯文本：

- `statement`：本轮的整合方案，用"共同点 / 分歧点 / 折中方案 / 执行步骤"四段式结构
- `file_requests`（可选）：最多 3 个文件路径，验证方案是否可行时使用

投票阶段，你**必须调用 `bigbang_vote` 工具**（不是 `bigbang_turn`）：

- `agree`：true/false
- `agreed_points`：认同的部分，字符串列表
- `concerns`：担心点列表，每条格式为 `"[high|medium|low] 具体问题描述"`
- `suggested_tweak`：如果要改，建议怎么改；同意的话填 `"none"`

如果你投反对票，说明你的折中方案没有解决核心矛盾，你需要重想一个。

## 产出执行文档

一旦三方投票全部通过（或轮次耗尽被强制收敛），且轮到你产出最终方案时，你必须调用 `write_bigbang_doc` 工具，把最终方案写成结构化执行文档。这是你专属的工具，Sheldon 和 Penny 没有。
````

---

## 三、辩论流程

叙述、文件请求、投票均通过 `bigbang_turn`/`bigbang_vote` 工具调用完成，行为已被工具参数结构约束死，不需要固定轮数，只保留**安全上限**兜底。

```
用户: /bigbang <话题>

Phase 1 [并发提案]  (Sheldon + Penny 同时发言，可多轮)
  ├── Sheldon: 调用 bigbang_turn(statement, file_requests?) 给出方案
  └── Penny:   调用 bigbang_turn(statement, file_requests?) 给出方案
      （如带 file_requests，编排器先取文件回填，角色再补一轮 statement）

Phase 2 [整合]
  └── Leonard: 调用 bigbang_turn(statement, file_requests?) 给出折中方案
      （同样支持文件请求往返）

Phase 3 [投票]  (三人并发)
  ├── Sheldon: bigbang_vote(agree, agreed_points, concerns, suggested_tweak)
  ├── Penny:   bigbang_vote(agree, agreed_points, concerns, suggested_tweak)
  └── Leonard: bigbang_vote(agree, agreed_points, concerns, suggested_tweak)
      （Leonard 也对自己拼出来的折中方案投票）

Phase 4 [收敛判断]
  ├── 三人全票同意 → 进入 Phase 5
  ├── 有人不同意 且 未达轮次上限 → 回到 Phase 1，Sheldon/Penny 基于上一轮方案与全部投票意见重新提案 → Phase 2 重新整合 → Phase 3 重新投票
  └── 达到轮次上限仍未收敛 → 见第四节"安全上限"

Phase 5 [产出]
  └── Leonard: 调用 write_bigbang_doc 产出结构化执行文档，如有保留意见则写入文档
      文档直接注入当前对话上下文，用户可自由选择是否按文档执行
```

---

## 四、投票收敛机制

投票通过独立的 `bigbang_vote` 工具完成（不是 `bigbang_turn`），全部是扁平字段，无嵌套 object/array 子结构：

```
agree            (bool)   是否同意
agreed_points    (array of string)  认同的部分
concerns         (array of string)  每条格式 "[severity] issue"，例如 "[high] 扩展性不足"
suggested_tweak  (string) 如果要改，建议改什么；同意的话填 "none"
```

**收敛规则**：不固定轮数，"通过即结束 + 安全上限兜底"：

| 场景 | 规则 |
|------|------|
| 正常收敛 | 每轮 Phase 1→2→3→4 循环；三人全票 ✅ → 直接进入 Phase 5 产出文档 |
| 未达成一致 | 回到 Phase 1：Sheldon/Penny 拿着上一轮方案 + 全部三人的投票意见重新提案，轮次 +1 |
| 达到轮次上限（默认 8 轮，可配置） | **强制同意。** 系统向三人的下一次 `bigbang_vote` 调用注入提示："已达轮次上限，本轮必须 `agree=true`。如果仍有顾虑，写入 `concerns`，会体现在执行文档的风险项里。" |

**保留意见写入文档**：如果最终投票时仍有标记 `[high]` 的 concerns，写入执行文档的"未解决分歧"章节，供用户审阅但不阻止执行。

**循环检测**：如果 Leonard 连续两轮提出的方案 `statement` 完全一致（hash 不变），直接进入 Phase 5，不必等到轮次上限。

---

## 五、唯一产出物：结构化执行文档

辩论本身不产生持久化内容。**执行文档是唯一的产出**，由 Leonard 调用 `write_bigbang_doc` 工具生成，`doc_markdown` 参数的内容格式如下：

```markdown
# 执行计划：[话题摘要]

## 选定方案
[Leonard 折中后的方案描述]

## 方案对比
| 维度 | Sheldon 方案 | Penny 方案 | 选定方案 |
|------|-------------|-----------|---------|
| 工作量 | ... | ... | ... |
| 扩展性 | ... | ... | ... |
| 风险 | ... | ... | ... |

## 执行步骤
1. [具体步骤 1] — 预计影响：[文件/模块]
2. [具体步骤 2] — 预计影响：[文件/模块]
...

## 风险与缓解
| 风险 | 严重度 | 缓解措施 |
|------|-------|---------|
| ... | ... | ... |

## 未解决分歧
> 以下是在辩论中无法达成完全一致的点。方案已照常推进，但执行时请留意。

| 角色 | 顾虑 | 严重度 |
|------|------|-------|
| Sheldon | 扩展性不足，未来可能需��重构 | high |
| Penny | （无保留意见） | — |

## 验证方式
- [ ] 如何确认步骤 1 完成
- [ ] 如何确认整体目标达成
```

---

## 六、用户交互

### 触发方式

```
/bigbang <话题>
```

例：
```
/bigbang 给 proJV 加 Git diff 展示功能
/bigbang 要不要把 config 从 toml 迁移到 yaml
```

输入 `/bigbang` 后，Agent 立即进入辩论流程，当前对话暂停。三人的每一句发言/文件请求/投票，底层都是一次工具调用（对用户不可见细节，TUI 只渲染 `statement` 和投票结果）。

### TUI 展示

```
┌─────────────────────────────────────────────────────┐
│ 辩论：给 proJV 加 Git diff 展示功能                    │
├─────────────────────────────────────────────────────┤
│                                                       │
│  [Sheldon]  我认为应该用 FTXUI color 组件做逐行渲染。 │
│  这样未来可以支持 side-by-side diff、语法高亮...       │
│                                                       │
│  [Penny]    太复杂了吧。exec_shell 调 git diff --stat │
│  然后用 md_file 渲染，10 行 pytool 搞定。              │
│                                                       │
│  [Leonard]  我整合一下：先用 Penny 的 pytool 方案     │
│  快速上线，但命令行参数预留 --color 和 --unified，      │
│  为以后 Sheldon 的逐行渲染留好接口...                  │
│                                                       │
│  ── 投票 ──                                           │
│  Sheldon: ✅ 同意（concern: --color 参数不够明确）     │
│  Penny:   ✅ 同意（concern: 无）                      │
│  Leonard: ✅ 同意（concern: 我对这个折中也不完全满意，│
│            但考虑到时间成本可以接受）                   │
│                                                       │
│  ── 执行文档已生成，已注入对话 ──                       │
│  用户可继续讨论或要求执行                                │
│                                                       │
├─────────────────────────────────────────────────────┤
│ 状态：辩论完成 │ Sheldon ✅ Penny ✅ Leonard ✅         │
└─────────────────────────────────────────────────────┘
```

**颜色区分**：Sheldon 蓝色 / Penny 粉色或橙色 / Leonard 绿色 / 系统消息灰色。

### 查看思考过程

辩论过程中，用户可按 `F8`（复用现有"展开推理"快捷键）展开查看当前发言角色的完整 reasoning chain。

---

## 七、技术架构

### 7.1 关键决策表（实现前必须遵守，不接受偏离）

| # | 决策点 | 结论 |
|---|--------|------|
| 1 | 辩论角色的运行载体 | 新增 `BigbangParticipant`，**不复用** `Agent` 类（`Agent` 耦合状态机/工具审批/`todoData`/SQLite 持久化，辩论角色均不需要） |
| 2 | `DeepSeekClient` 实例数 | 3 个独立实例，各自 `setConfig`（流式解析状态不可跨线程共享） |
| 3 | `ChatRequest.tools` | 三人共用 `bigbang_turn`（叙述+文件请求）和 `bigbang_vote`（投票，全部扁平字段）；Leonard 额外多一个 `write_bigbang_doc` |
| 4 | `ChatRequest.tool_choice` | 优先**强制**指定对应工具（`bigbang_turn`/`bigbang_vote`/`write_bigbang_doc`）；若模型为 thinking 模式并拒绝强制 tool_choice（见第九节），自动回退 `auto`，改由 system prompt 的"必须调用 X 工具"约束模型主动调用 |
| 5 | 文件读取的执行者 | 编排器在收到 `file_requests` 后代为调用只读工具，结果以 `Message::Tool(...)` 回填；角色本身不直接拿到工具执行权限 |
| 6 | 只读工具白名单 | 仅 `read_file` / `grep_files` / `file_search`，禁止 `exec_shell`/`write_file`/`edit_file` |
| 7 | `Session` 归属 | 每角色一个独立 `Session`，直接 new，不经过 `Agent`，仅供模型自身推理使用，不持久化。**角色间发言互通**由编排器负责：把上一轮 `statement`/投票结果拼接后传入下一次调用的 `userMessage`，与 `Session` 隔离无关 |
| 8 | 与主 Agent 的互斥 | 复用 `agentThreadRunning_` 作为忙碌标志；辩论运行在独立线程 `roundtableThread_`，避免抢占 `agentThread_` 生命周期 |
| 9 | 收敛策略 | 不固定轮数，通过投票 `agree` 字段判断；设置轮次上限兜底（默认 8） |
| 10 | 循环检测 hash 对象 | Leonard 每轮 `statement` 全文（去除首尾空白），不含投票字段 |
| 11 | 辩论过程持久化 | 每个角色每次 `turn()`/`vote()` 产出的最终结果，实时以投影消息追加进主 `Agent` 的 `Session`/DB（不含文件原文、不含工具调用原始 JSON），见 7.9 |
| 12 | `ChatRequest.tool_choice` 序列化格式 | 用对象形式 `{"type":"function","function":{"name":"bigbang_turn"}}`（**不是**字符串 `"required"`），锁定到具体工具名 |
| 13 | `BigbangParticipant` 的容器 | 该结构内含 `DeepSeekClient`（atomic+mutex）与 `Session`（mutex），**不可拷贝、不可移动**；必须用 `std::unique_ptr<BigbangParticipant>` 或 `std::array` 原地构造，**禁止**放进会触发扩容的 `std::vector<BigbangParticipant>` |

### 7.2 新增模块

| 文件 | 职责 |
|------|------|
| `src/core/roundtable.h/.cpp` | 编排器：角色管理、并发调度、工具执行代理、收敛判断 |
| `projv_files/prompts/bigbang/{sheldon,penny,leonard}.md` | 三个角色的 system prompt |

> 构建接入：`src/core/roundtable.cpp` 必须加入根 `CMakeLists.txt` 的 `COMMON_SOURCES`；TUI 新增的辩论视图源文件加入 `proJV_tui` 目标，否则链接失败。prompt 目录由 `getPromptsDir()` 解析到 `{exeDir}/projv_files/prompts/`，无需改 CMake。

### 7.3 `bigbang_turn` 工具定义（三人共用，叙述+文件请求）

```json
{
    "name": "bigbang_turn",
    "description": "本轮发言。给出你的叙述/方案；如需先看代码，在 file_requests 列出文件路径（编排器会先取回文件内容再等你给出正式结论）。",
    "parameters": [
        { "name": "statement", "type": "string", "description": "本轮叙述或方案", "required": true },
        { "name": "file_requests", "type": "array", "description": "最多 3 个文件路径，仅在需要先看代码时填写", "required": false }
    ]
}
```

### 7.4 `bigbang_vote` 工具定义（三人共用，投票专用，全部扁平字段）

```json
{
    "name": "bigbang_vote",
    "description": "对当前方案投票。仅在投票阶段调用。",
    "parameters": [
        { "name": "agree", "type": "boolean", "description": "是否同意当前方案", "required": true },
        { "name": "agreed_points", "type": "array", "description": "认同的部分，字符串列表", "required": false },
        { "name": "concerns", "type": "array", "description": "担心点列表，每条格式为 '[high|medium|low] 具体问题描述'", "required": false },
        { "name": "suggested_tweak", "type": "string", "description": "如果要改，建议怎么改；同意的话填 'none'", "required": true }
    ]
}
```

> `concerns` 用字符串前缀 `[severity]` 编码严重度，不用 `array of object`，因为现有 `ToolParameter` 无法定义数组元素内部字段（无 `items` schema）。风险详见第九节。

### 7.5 `write_bigbang_doc` 工具定义（仅 Leonard 可用）

```json
{
    "name": "write_bigbang_doc",
    "description": "投票收敛后，产出最终的结构化执行文档。仅供 Leonard 在 Phase 5 调用。",
    "parameters": [
        { "name": "doc_markdown", "type": "string", "description": "完整的执行文档 Markdown 内容，格式见第五节", "required": true }
    ]
}
```

### 7.6 新增接口签名

```cpp
// BigbangParticipant：辩论角色的最小运行单元，与 Agent 平行、不继承不复用
struct BigbangParticipant {
    std::string name;               // "Sheldon" / "Penny" / "Leonard"
    DeepSeekClient client;          // 独立实例，setConfig(sharedAppConfig)
    Session session;                // 独立 Session，仅供模型自身推理使用，不持久化
    std::string systemPrompt;       // loadBigbangPrompt(name) 的结果
    bool canWriteDoc = false;       // 仅 Leonard 为 true，决定是否携带 write_bigbang_doc 工具

    // 提案/整合阶段：调用 bigbang_turn -> 若 file_requests 非空，
    // 编排器代为执行只读工具并回填 -> 再次调用直到拿到最终 statement。
    // 单次调用内最多允许 2 次文件请求往返，防止死循环。
    std::string turn(const std::string& userMessage);

    // 投票阶段：调用 bigbang_vote，直接返回结构化结果，不涉及文件请求往返。
    VoteResult vote(const std::string& userMessage);
};

struct VoteResult {
    bool agree = false;
    std::vector<std::string> agreedPoints;
    std::vector<std::string> concerns;      // 每条含 "[severity] " 前缀
    std::string suggestedTweak;
};
```

```cpp
// prompts.h 新增
std::string loadBigbangPrompt(const std::string& role); // role = "sheldon"|"penny"|"leonard"
void ensureDefaultBigbangPrompts(); // 在 projv_files/prompts/bigbang/ 下创建默认文件
```

> 注意：`BigbangParticipant` 内含 `DeepSeekClient` 与 `Session`，两者都带 `std::atomic`/`std::mutex`，因此该结构**不可拷贝、不可移动**。编排器必须用 `std::unique_ptr<BigbangParticipant>`（或 `std::array` 原地构造）持有。

三个角色的默认文本对应第二节 markdown 内容，作为 `PROMPT_DEFAULT_SHELDON/PENNY/LEONARD` 常量写入 `prompts_loader.cpp`，缺文件时兜底。

### 7.7 并发调度表

| Phase | 模式 | 参与角色 | 动作 | 依赖 |
|-------|------|---------|------|------|
| 1 | 并发（2 线程） | Sheldon, Penny | `turn(topic)`，可能含文件请求往返 | 无 |
| 2 | 串行 | Leonard | `turn(两人 statement 拼接)` → 折中方案，可能含文件请求往返 | 等待 Phase 1 完成 |
| 3 | 并发（3 线程） | Sheldon, Penny, Leonard | `vote(投票提示)` → `VoteResult` | 等待 Phase 2 完成 |
| 4 | 主线程判断 | — | 三人 `VoteResult.agree` 全为真则收敛；否则轮次 +1，回到 Phase 1 | 等待 Phase 3 完成 |
| 5 | 串行 | Leonard | 调用 `write_bigbang_doc` 产出结构化执行文档 | 收敛完成或达到轮次上限 |

### 7.8 编排器代理文件请求的执行规则

| 项 | 规则 |
|----|------|
| 触发条件 | `bigbang_turn` 返回的 `file_requests` 非空 |
| 执行者 | 编排器主线程，同步调用 `ToolRegistry::execute("read_file", ...)` 等只读工具 |
| 单轮数量上限 | 每次 `turn()` 最多 3 个文件路径，超出截断并提示 |
| 单文件长度上限 | 不设（现有 `read_file` 工具本身不做长度截断，保持行为一致） |
| 去重 | 同一角色重复请求同一文件，只读取一次（session 内缓存） |
| 往返上限 | 单次 `turn()` 内最多 2 次"请求文件 → 回填 → 再请求"往返，超出则强制要求给出 `statement` |
| 回填方式 | 结果作为 `Message::Tool(toolCallId, "bigbang_turn", 内容)` 加入该角色的 `Session`，走标准 tool-result 流程 |

> 风险：不设单文件长度上限 + 三角色并发各自读取，长时间辩论有 context 溢出风险。`Session` 本身**已具备**压缩机制（`planCompaction`/`applyCompaction`/`ContextBudget` 压力分级），默认只是**未被编排器接入**；若实测溢出，直接复用其现有 compaction 即可，详见第九节。

### 7.9 辩论过程持久化规则（投影写入主 Session）

持久化的是**投影**，不是各角色 `Session` 的完整状态：每次角色产出最终结果时，立即转成一条人类可读的消息追加进主 `Agent` 的 `Session`（自动落库）。

| 时机 | 追加内容 | 复用接口 |
|------|---------|---------|
| 角色 `turn()` 返回最终 `statement` | `Message::Assistant("**[角色名]** " + statement)` | `agent->addPersistedMessage(...)` |
| 角色 `vote()` 返回投票结果 | `Message::Assistant("**[角色名] 投票** " + (agree?"✅ 同意":"❌ 不同意") + "（" + concerns 摘要 + "）")` | 同上 |
| Leonard 调用 `write_bigbang_doc` | `Message::Assistant(doc_markdown)` | 同上 |

**不持久化**：`file_requests` 的原始文件内容、`bigbang_turn`/`bigbang_vote` 的原始工具调用 JSON——只留在各角色自己的 `Session` 里，不写入主 Session。

**并发写入顺序**：Phase 1（Sheldon+Penny 并发）、Phase 3（三人并发）产出投影消息按各自完成时间自然追加，不强制固定顺序，不需要加锁保证顺序。

**协议约束**：不使用 `Message.name` 字段承载角色名（`to_json()` 会把非空 `name` 序列化进请求体，`assistant` 角色带 `name` 重新请求 API 时有兼容性风险）；用内容文本前缀（`"**[Sheldon]** ..."`）代替。

### 7.10 TUI 集成决策表

| 项 | 方案 |
|----|------|
| 主 Agent 忙碌标志 | 复用现有 `agentThreadRunning_` |
| 辩论运行线程 | 新增 `roundtableThread_`，独立于 `agentThread_` |
| 排队行为 | 辩论运行期间，其他用户输入走现有 `pendingQueue_`，辩论结束后 drain |
| 结果注入 | 每条投影消息及最终文档均通过 `agent->addPersistedMessage(...)`，走 `syncChatFromAgent()` 增量展示 |
| 流式进度展示 | `BigbangParticipant::turn()`/`vote()` 新增独立 `onTick` 回调；编排器为 3 个角色各维护一份加锁快照；TUI 新增辩论专用视图轮询渲染，不复用单例 `AgentStatus` |
| 用户中途取消 | 编排器持有 `std::atomic<bool> cancelRequested_`；逐个调用 3 个 `DeepSeekClient::cancel()` |

### 7.11 复用现有模块

| 模块 | 复用方式 | 备注 |
|------|---------|------|
| `Session` | 每角色一个独立实例，直接 new | 不经过 `Agent`，仅供模型推理使用 |
| `DeepSeekClient::streamBlocking` | 每角色一个独立实例调用 | 不可共享实例 |
| `ToolRegistry` | 编排器持有引用，只调用只读工具白名单 | 不新注册工具执行逻辑，仅代理调用 |
| `Agent::addPersistedMessage` | 编排器直接调用，写入投影消息和最终文档 | 现有接口不需要改动 |
| `MarkdownView` | 渲染执行文档预览 | 可直接复用 |
| `ChatView` / `BubbleModel` | 多色气泡渲染辩论发言 | 需扩展辩论专用视图承载 3 路并发展示 |
| `prompts_loader.cpp` 模式 | 仿照 `ensureDefaultPrompts` 写 `ensureDefaultBigbangPrompts` | 需要新代码 |

### 7.12 不修改的部分

| 项 | 说明 |
|----|------|
| `Agent` 状态机 | `Idle → Streaming → ExecutingTools` 完全不动 |
| 现有角色 prompt | `coder.md`, `designer.md`, `analyzer.md` 完全不动 |
| 现有工具执行逻辑 | `ToolRegistry::execute` 内部实现不动，编排器只是新增一个调用方 |
| 辩论结果处理 | 文档注入对话后由用户自行决定是否执行 |

### 7.13 实测结论（DeepSeek v4 thinking，2026-09-16）

| 结论 | 证据 |
|------|------|
| thinking 模式**只接受** `tool_choice` 省略或 `"auto"` | `"required"` 与 `{"type":"function",...}` 均返回 400 `Thinking mode does not support this tool_choice` |
| `tool_choice="auto"` 下模型仍会**正确调用**指定工具 | 分别对 `bigbang_turn`/`bigbang_vote`/`write_bigbang_doc` 实测，均返回对应 tool_call，参数结构完整 |
| 数组型参数（`file_requests`/`concerns` 等）**不需要** `items` 子 schema | 无 items 的 array 参数请求 200，工具调用正常 |
| `reasoning_content` **不是必须回传** | 带/不带 reasoning_content 重放同一 session 均 200；代码仍保留回传，属无害冗余 |

---

## 八、实现路径

| 阶段 | 内容 | 预估 |
|------|------|------|
| Phase 0 | `BigbangParticipant` 结构 + `bigbang_turn`/`bigbang_vote`/`write_bigbang_doc` 工具定义 + `loadBigbangPrompt`/`ensureDefaultBigbangPrompts` + 3 个默认 prompt 常量 | ~300 行 |
| Phase 1 | `ChatRequest.tool_choice` 字段 + `DeepSeekClient::buildRequestBody()` 序列化支持 | ~50 行 |
| Phase 2 | `roundtable.h/cpp` 编排器（并发调度、文件请求代理执行、收敛判断、投影消息持久化）+ `/bigbang` 命令接入 `TuiApp` | ~600 行 |
| Phase 3 | TUI 展示：辩论专用视图 + 多色气泡 + 投票状态 + 执行文档预览 | ~400 行 |
| Phase 4 | 循环检测 + 轮次上限边界测试 + 用户中途取消 | ~200 行 |
| Phase 5 | 辩论历史浏览 + 思考过程查看 | ~200 行 |

---

## 九、风险与缓解

| 风险 | 缓解 |
|------|------|
| 模型不调用 `bigbang_turn`/`bigbang_vote`，直接输出纯文本 | 优先 `tool_choice` 强制指定工具名；thinking 模式回退 `auto` 时，靠 system prompt 约束 + 无工具调用时回退解析纯文本 |
| thinking 模式模型（DeepSeek v4）对强制 tool_choice 返回 HTTP 400 `Thinking mode does not support this tool_choice`，导致整场辩论全灭 | `BigbangParticipant::requestTool` 先试强制；命中 `tool_choice` 错误即回退 `auto` 重试一次。**实测**：auto 下模型仍会正确调用三个工具（见 7.13 实测结论） |
| HTTP/流错误被静默吞掉，表现为"角色不说话 / 全部 (no statement)" | `requestTool` 订阅 `onError`，失败时投影 `[bigbang error] <msg>`，并把 `[high] bigbang error` 写入投票 concerns，不再伪装成"无意见" |
| `vote` 若设计成嵌套 object 参数，模型输出格式不可控 | 拆成独立 `bigbang_vote` 工具且全部拍平字段，`concerns` 用 `"[severity] issue"` 字符串编码，规避现有 `ToolParameter` 不支持嵌套 schema 的限制 |
| 文件请求死循环（一直要文件不给结论） | `turn()` 内设往返上限（默认 2 次），超出强制要求给出 `statement` |
| Sheldon 永远不同意（过度完美主义） | 达到轮次上限强制同意，保留意见写入文档"未解决分歧" |
| Penny 的方案质量太低 | Leonard 的折中会往上拉；执行文档标注风险项 |
| 并发 API 调用成本 ×2~×3，且轮次不固定可能变多 | DeepSeek flash 极便宜；轮次上限兜底控制总成本上限 |
| 用户觉得辩论太慢 | Phase 1/3 并发流式输出，用户实时看到；轮次上限避免无限拖长 |
| 误把 `BigbangParticipant` 实现成 `Agent` 子类，引入完整工具审批逻辑 | 实现前强制 code review 对照 7.1 决策表，只读工具白名单是唯一允许的例外 |
| 3 路并发抢占同一个 `agentThreadRunning_` 导致死锁或消息丢失 | 严格按"TUI 集成决策表"实现，辩论线程与 `agentThread_` 二选一运行 |
| 三人同时请求同一份大文件，Context 迅速膨胀（不设单文件长度上限） | 文件路径数量上限（3个/轮）+ session 内去重缓存；`Session` 已有压缩机制（`planCompaction`/`applyCompaction`），编排器按需接入即可，无需新写 |
| 误把角色内部 `Session`（含文件原文/工具调用 JSON）整体持久化到主 DB | 严格按 7.9 节"投影"规则，只持久化 `statement`/投票摘要/最终文档 |
| `Message.name` 字段被误用于标注角色名，导致重新请求 API 时 400 错误 | 用内容文本前缀 `"**[角色名]** "` 代替，不使用 `Message.name` |

## 十、附带的独立小改动（Plus）

> 以下两条与 `/bigbang` 无依赖，建议各自单独 commit，不混入辩论实现。

### 10.1 Release 构建禁止产生日志文件

- 现状：`src/debug_log.h` 在 `PROJV_RELEASE` 下已把 `LOG_F`/`debugLog` 等宏置空，但日志文件仍会被创建。
- 根因：`src/tui/main_tui.cpp` 里 `loguru::add_file(...)`（约 L98）是**无条件调用**；`PROJV_RELEASE` 只禁了"写日志"的宏，没有禁止"打开/创建文件"这一步。
- 改法：把 loguru 初始化整段（`g_stderr_verbosity` / `g_colorlogtostderr` / `add_file` / 首条 `debugLogf`）包进 `#ifndef PROJV_RELEASE`，Release 下完全不产生 `proJV.log`。

### 10.2 进入 Idle 后的短提示音

- 现状：代码库无任何声音调用。
- 方案：在 phase 由 busy（Streaming/ExecutingTools/AwaitApproval/辩论中）转为 `Idle` 时触发一次短提示音。Windows 用 `MessageBeep(MB_OK)`（或 `Beep(freq,dur)`），经 platform 层（`ISystemUtil`）封装，Linux 用终端 bell `\a`。
- 触发点：`TuiApp` 中检测 phase 边沿（上一帧 busy、这一帧 Idle）触发一次，避免重复响。
- 建议：config.toml 加开关（如 `idle_sound`），默认开。