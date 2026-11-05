# TEMP 设计稿：提示词单角色化 + 技能通用化 + OSC 7501 对齐 + monica 基线

> **TEMP** —— 中文、仅供评审，未定稿。不进 `docs/en/` 镜像；定稿后拆成双语正式文档，本文件删除。
> 本文只做设计。行号均经实读；未核实标 `[待证]`。
> 2026-11-05 对照代码复读后订正：工具可见性分三类（pytool 动态注册）、C2 与 clear 的字段顺序、旧三份的隐藏名单、en+zh `03-roles.md` 进改动清单、bigbang 写死中文。C1 已落地代码。

---

## 0. 目标

| 代号 | 工作项 | 一句话 |
|---|---|---|
| A | 提示词单角色化 | 四份角色提示词收成一个 universal `assistant.md`；plan→execute 用 TODO + 确认门约束，不丢 hard rules |
| B | 技能通用化 | bigbang / avenger 从"代码/市场专属"扩到任意工程师/设计师/写作等题材；输出先白话后细节 |
| C | OSC 7501 对齐 0.4 | 修不合规处（C1 必改，C2 建议，C3 拍板，C4 待核） |
| D | monica 巡检 | 在 A/B 之后跑，产出 MCP 实施前的代码基线报告 |

贯穿约束：所有提示词与技能输出遵循**两层表达**——① 白话结论（本科生毕业也能理解）→ ② 专业细节（文件路径、签名、数据、参数）。替代现 `Keep responses concise` + "conclusion first + plain language"（`prompts_loader.cpp:21, 30-33`）三条，它们只约束"简洁"与"顺序"，不约束受众可理解度。

**叙述语言让 LLM 自判，不写死**：删 `prompts_loader.cpp:67/:161` 的 `Chinese for narrative; English for code`（写死中文，英文用户拿到中文报告），改为：**匹配会话与项目语言**（AGENTS.md / README / 代码注释的多数语言），无法判定时镜像用户最近一条消息的语言。代码本体、文件路径、函数名永远保持原样（不翻译）。`prompts_loader.cpp:23` 的 "code ONLY English and ASCII" 保留——代码本体必须英文，任何语言使用者都成立。

---

## 1. 现状事实

- proJV 无角色系统：四份提示词只是 `prompts_loader.cpp` 启动时写的纯文本（presets `:329-334`）。
- 提示词里 `tools available:` 行是**权限表**：`agent.h:100` `reloadAllowedTools()` → `tool_list_parser.cpp` 按字面量解析；名单外工具模型看不到（`agent.cpp:931, 959-964`）。
- **动态切换 system prompt 已存在**：`Agent::replaceSystemPrompt`（`agent.h:71-72`，注释明确 "role switching without clearing context"）→ `TuiApp::switchPrompt`（`app_tui.cpp:958-965`）→ Tab 循环（`main_tui.cpp:413-419`）。双角色方案不需要新机制。
- 静默失效点：
  1. `agent.cpp:959-964`：`allowedTools_` 为空 = 全放行，漏写工具行不报错。
  2. `app_tui.cpp:253-257`：默认角色按字面量找 `"coder.md"`，不是列表首位。
  3. `app_tui.cpp:259`：`promptFiles_` 为空时越界（顺手修）。
- 工具可见性分三类，别混为一谈：
  1. `format_file`（`format_tool.cpp:130`）、`list_dir`（`file_tool.cpp:504`）已注册，三份主角色提示词都不带 → **主 agent** 用不了；但 monica 的工具白名单带（`skills/monica/config.json:5`），skill 子角色今天就在用。它们只在"主角色"意义上是孤儿。
  2. pytool 是动态注册：`projv_files/pytool/<name>/tool.json` 由 `python_tool_manager.cpp:174` 扫盘注册，`diagram_tool` 本身就是 pytool，不是内置工具。新增一个 pytool 目录，模型在改工具行之前看不到它 → "注册表 ⊆ 某角色工具行"必须**启动后动态比对**，写不成静态清单。
- 表达规则现状：`prompts_loader.cpp:21` `Keep responses concise` 与 `:30-33` "conclusion first + plain language" 只鼓励浓缩结论与顺序约束，无受众标准；本轮用"本科生毕业也能理解"替代。`prompts_loader.cpp:67/:161` 的 `Chinese for narrative` 写死叙述语言（英文用户会拿到中文报告），改为 LLM 自判。
- skill 工具白名单是真闸门：`sub_agent.cpp:186-189` 执行时过滤，`skill_runner.cpp:393-394` 按名单注册 schema。bigbang 现只有 `read_file, grep_files, file_search`，零联网。

---

## 2. 工作项 A：一个 universal assistant.md

### 2.1 方案

单文件 + 工作流约束。plan→execute 不靠文件切换（虽然机制存在，但双文件换来的只是"少四个工具名"，还要养一套阶段切换 UI 语义）：

- 计划 = `update_todo(action="init", tasks=...)` + 确认门（先白话说清理解 + 交付物，歧义先确认）
- 执行 = `update_todo(action="update", ...)` 逐步推进，完成 `action="done"`；TODO 状态经 `todo_tool.cpp:277-296` 注入 system 块（`agent.cpp:934-936`），右侧面板可见（`main_tui.cpp:518`）

**assistant.md 不是"新的 coder"，是取三份优点整合**——todo 只是进度载体，不是准则来源。三份各有独有准则，缺了会退化：

| 来源 | 独有准则（另外两份没有） |
|---|---|
| coder | 8KB 上限、exec_shell timeout、批量工具调用、run_skill/view_image 纪律、英文代码 |
| designer | 具体到文件路径+签名+行数、表内禁空行、md_file 专属写文档、设计文档五段结构、线程安全/资源生命周期检查、先读全部源码再写 |
| analyzer | diagram_tool 绘图准则（digraph/rankdir/cluster）、数据流追踪、图表类型约定 |

**"要求先白话结论"与"Keep responses concise"合并成一条可判定标准**：
- 删 `prompts_loader.cpp:21` `Keep responses concise -- compact conclusions`
- 删 `:30-33` 三层表达/plain language 两条，替换为一条：**"用本科生毕业也能理解的语言给出结论——先讲清是什么和它对你意味着什么，再给专业细节"**
- 删 `:67/:161` `Chinese for narrative; English for code...`，替换为：**匹配会话与项目语言（AGENTS.md / README / 代码注释多数语言），无法判定时镜像用户最近一条消息的语言；代码/路径/函数名保持原样**（`buildAgentsMdContext` `app_tui.cpp:880` 已注入 AGENTS.md，模型有现成信号）
- 保留 designer 的 `- Be concrete -- exact file paths...`（否则模型只写大白话、丢工程细节）与 coder 的 "code ONLY English and ASCII"（代码本体必须英文，任何语言使用者都成立）

工具行（15 个）：现 coder 13 个 + `list_dir`（规划） + `format_file`（执行收尾）。

### 2.2 改动清单

| 文件 | 动作 |
|---|---|
| `prompts_loader.cpp` | presets 表换成 `assistant.md` 一个种子；删 coder/designer/analyzer 种子；`upgradeSeededPrompt`（:290-298）补分支走 `upgradeStyleText`（不补 styles 测试首轮红）；`upgradePromptFile`（:349-352）改一行 |
| `prompts.h` | 加 `defaultPromptSeed(name)`、`defaultPromptIndex(list)`；顺带改 `:6-7` 注释里的 `"coder.md"`（§8 的 grep 验收会命中这两行注释） |
| `app_tui.cpp` | `:253-257` 换 `defaultPromptIndex`；`:250-251` 的 erase 名单**只留 `compactor.md`**（复核后推翻原方案：按名字隐藏旧三份等于禁止用户再用这三个名字，属于把实现细节变成用户约束）；`:259` 空表守卫（`:295`、`:901` 是同一处裸索引，一并防） |
| `docs/en/user-guide/03-roles.md`、`docs/zh/user-guide/03-roles.md` | 角色表（en `:12-19`、zh `:14` 起）改成 assistant + compactor；顺手订正 en `:42-43` 那句错话——真实现状是 `agent.cpp:959-967` 过滤 + `:819-820` 拒调，工具行就是权限表，不是"skill 级设置、主 agent 拿全套工具" |
| `test_prompt_style.cpp` | files[] **追加** assistant.md，不要删 coder/designer/analyzer —— `upgradeSeededPrompt` 的旧分支必须留给老安装，测试删了就没人守它；工具集合相等断言；旧角色名不在 assistant.md 断言 |

**compactor.md 保留**：`/compress` 的 LLM 压缩提示词（`agent.cpp:787`），不属于主角色范围。

### 2.3 新种子必须包含（缺失即种子被改坏）

1. 确认门（继承 `prompts_loader.cpp:27-29`）
2. 计划先行：多步骤先 `update_todo(init)`；执行推进：每步 `update_todo(update)`，完成 `done`；不做计划外工作
3. 表达规则（合并原三条）：**结论用本科生毕业也能理解的语言**，先讲清是什么与它对你意味着什么，再给专业细节；**叙述语言匹配会话与项目语言**（AGENTS.md/README/代码注释多数语言，无法判定时镜像用户最近消息语言），代码/路径/函数名保持原样；保留 `Be concrete -- exact file paths`
4. 继承 coder/designer/analyzer 独有准则：8KB、exec_shell timeout、批量调用、run_skill/view_image 纪律、md_file 专属写文档、五段结构、线程安全/资源检查、diagram_tool 绘图准则、先读全部源码再写、code ONLY English and ASCII

### 2.4 硬约束

- 不删用户文件：coder/designer/analyzer 留磁盘；它们就是普通角色（文件名即角色名），只有 `compactor.md` 是内部提示词、不进循环
- 不做等值迁移；断言成对（测"不含"旧角色名）
- compactor.md 不套三层表达（压缩是结构化抽取）

### 2.5 已知缺口

- 计划无独立文件形态（`md_file` 强制 `.md` + 8KB）；持久形态 = TODO 面板 + 会话库
- 阶段边界只在提示词文本，无代码强制

---

## 3. 工作项 B：bigbang / avenger 通用化

### 3.1 必改点

1. **证据规则扩源**：bigbang 三份角色各有同款证据规则（`sheldon.md:26-27`、`penny.md:27-28`、`leonard.md:19-20`）"Any claim about the codebase... MUST come from a file you read THIS round" 在通用调研（无文件可引）下会一路否决到打满 `max_rounds`。扩成"本轮读过的文件或 URL/检索结果"，保留"未核实标 `[assumed]`"。
2. **输出给谁看**：avenger `stark.md:77-117` B1-B4 全是"创业者/投资人"语言；七条 worker 线（`research_{mkt,user,oss,tech,comp,money,eco}`）与 `research_mkt` 的 market/price 全是单一受众假设；bigbang 三份提示词 "design document/codebase" 全是代码语义。
3. **不预置 source catalog，先学后做**：用户对领域无知是默认假设。喂具体 URL/目录 = 预设答案，违背 universal。research_aud 增 **round 0 摸底**——从入门内容现学，产出**领域知识地图**：关键问题清单、每条线该找的**信息类型**（"许可监管页"——类型是方法，任何领域成立；具体 URL 是答案，不预置）、本地未知项。七线按地图分配深挖。
4. **领域推导判据，不用固定模板**：B 段"4 周最小成果"由摸底地图推导（咖啡店 = 摊位/pop-up + 选址流量计数；袜子 = 小批量 + 平台店），Stark 不得套固定模板。
5. **线下数据盲区显式化**：咖啡店的人流/租金/选址是本地数据，web_search 抓不到。地图与最终报告显式标"未知项"，模型不编数填坑。

### 3.2 改动清单

| 文件 | 动作 |
|---|---|
| `skills/bigbang/config.json` | tools 加 `web_search`、`fetch_url`；description 去 "engineering" |
| `skills/bigbang/{sheldon,penny,leonard}.md` | 证据规则扩"文件或 URL"；通用措辞；proposal 阶段加"先 5 行白话"；**删写死中文的长度要求**（`sheldon.md:49`、`penny.md:51`、`leonard.md:53` 的 "NNN chars in Chinese"）改成"匹配会话与项目语言"——否则 A 的语言策略和这三行正面对撞 |
| `skills/bigbang/tools/write_bigbang_doc.json` | description 改"方案文档"；结构要求实际在 `leonard.md:45-50`，一并改"先白话结论" |
| `skills/avenger/config.json` | agents 加 `research_aud`（round 0 摸底）；`round_steps` ROUND 1 改：先派 aud 摸底出地图，再按地图派七线 |
| `skills/avenger/research_*.md` | 证据规则扩源；报告开头加"白话摘要"（3-5 行）；mkt/comp/money 措辞改"题目相关"；接受 round 0 地图派发 |
| `skills/avenger/stark.md` | B1-B4 改"单人落地路径"，判据由领域地图推导、可判定 |
| `skills/avenger/README.md`、`skills/bigbang/README.md` | 声明适用任意题材；"先学后做"流程说明 |

### 3.3 风险

- web 工具拖长墙钟（`max_rounds: 8` 可能翻倍），无 per-tool 超时；round 0 摸底再吃一轮（约 5 次搜索 + 1 篇地图）
- `research_aud` 唤醒后 7 线全醒，`max_parallel: 6` 吃紧
- 通用题目下 `[assumed]` 标记变多（正常）
- B 段验收不改成可判定 → `DONE` 乱发
- 摸底轮只激活常识不验证 → 地图里的领域常识可能过时（无预置目录时无法避免；靠各线深挖时验证修正）

---

## 4. 工作项 C：OSC 7501 对齐 0.4

规范：`https://www.superlogical.com/rex/docs/build/program-status`（0.4 = 2026-10-10 最新）

| # | 要求 | 现状 | 判定 |
|---|---|---|---|
| C1 | 解码后 msg/title 禁 U+0000–U+001F、U+007F、U+0080–U+009F | `terminal_status.cpp:104` 逐字节判 `c<0x20 \|\| c==0x7f`；C1 字符（`C2 80`..`C2 9F` 两字节）漏网 → 整条报文被宿主丢弃 | **必修 → 已修**：`sanitizeControl` 改按码点（两字节塌成一个空格，不透传 `0xC2`），`test_osc7501.cpp` 补 C1 区间 + U+00A0 正例断言 |
| C2 | 每条报文必须重复 `app` | `:167` `if (!app.empty())` 才带；更麻烦的是 `:165` 对 `state=clear` 提前 return，`app` 在 `:167` 才追加 → 今天连 clear 都不带 app | 建议必填：只改 `:167` 的条件不够，得把 clear 的 return 挪到 app/id 之后 |
| C3 | 完成即退出应留 done/error | `shutdown()` 发 `state=clear`（`:230`）；但 phase→`done`/`error` 的映射已经接好（`app_tui.cpp:225-236`） | 能力不缺，缺的只是退出语义：正常退出留 clear，崩溃/Ctrl-C 走 `error` |
| C4 | 用户中断上报 `idle` | `Agent::cancel`（agent.cpp:545-555）不设 phase，但主循环各退出路径均落 `AgentPhase::Idle`，OSC 正常收敛 | 成立，无需改 |
| C5-C7 | ST 用 ESC\ / 特性探测可选 / 上限常量 | 合规 | 不动 |

---

## 5. 工作项 D：monica 巡检

`skills/monica/config.json`：1 supervisor + 10 worker，`max_parallel:16`、`max_rounds:10`，工具含 `write_file`/`edit_file`/`format_file` —— **会改仓库**。

- 目的：MCP 实施前基线（格式统一、中文注释清理、死代码清单、逐文件现状报告）
- **规则**：分支上跑；跑完看 `git diff --stat` 与死代码清单，不盲 merge；死代码只报告不删除
- 顺序：A、B 之后（否则 A/B 改过的文件会被再格式一遍）

---

## 6. 顺序

```
C（半天，独立可发）
  ↓
A（单角色） → B（技能通用化）
  ↓
D（monica，分支上）
  ↓
MCP 实施（见另一篇）
```

依赖：A 早于 MCP（MCP 的 roles 按提示词文件名做 key，需 assistant.md 存在）；D 在 A/B 后、MCP 前。

---

## 7. 风险表

| 风险 | 触发 | 严重度 | 立刻 | 长期 |
|---|---|---|---|---|
| 漏写 `tools available:` → 全放行 | 手写种子 | 高 | 工具集合相等断言 | `parseAvailableTools` 空结果报错 |
| 忘改 `app_tui.cpp:252-258` 字面量 → 新角色永不生效 | 只加文件不改代码 | 高 | 启动日志打印角色与工具数 | 默认角色进配置 |
| `upgradeSeededPrompt` 不加分支 → styles 测试红 | 只加文件名 | 中 | 同提交改分支 | dispatch 表驱动 |
| `promptFiles_` 空 → `:259` 越界 | 用户清空目录 | 中 | 空表守卫 | 索引走 `defaultPromptIndex` |
| 漏写两层表达 → 模型只给浓缩结论 | 手写种子 | 高 | 断言种子含"本科生毕业也能理解" | 种子 golden 化 |
| 叙述语言写死中文 → 英文用户拿到中文报告 | 继承 `Chinese for narrative` | 中 | 断言种子不含该句；种子含"匹配会话与项目语言" | 同上 |
| 旧角色名留工具行 → 权限意外收窄 | 复制改造 | 中 | 断言旧角色名不在 assistant.md | 同上 |
| OSC C1 放行 → 宿主丢弃整条 | 消息带 U+0080-9F | 中 | 按码点过滤 | C1 编码测试 |
| monica 在 main 跑并提交 | 直接执行 | 高 | 分支 + 人工看 diff | 技能只读模式开关 |
| 注册工具无人可用 | 加内置工具或加 `projv_files/pytool/` 目录，忘改工具行 | 中 | 本轮补进 assistant.md（`list_dir`/`format_file`） | 启动后动态跑"扫描注册表 ⊆ assistant.md 工具行"，pytool 才是常态来源 |
| 双语文档没跟上角色合并 | 只改代码 | 高 | 同提交改 en+zh `03-roles.md` | grep 校验文档里出现的角色名 |
| bigbang 加 web 后墙钟翻倍 | 通用调研题 | 低 | 文档标注 | skill per-tool 超时 |
| 7 线全醒 `max_parallel:6` 吃紧 | research_aud 唤醒 | 低 | 文档标注 | 上限按需调 8 |
| B 段验收不可判定 → `DONE` 乱发 | 只改名不改判据 | 高 | 判据写成可判定 | 同上 golden |

---

## 8. 验收

- `ctest -C Release --output-on-failure` 全绿；新增断言 ≥ 4（agent 集合相等、旧角色名不在 assistant.md、`defaultPromptIndex`、种子含"本科生毕业也能理解"）
- 手改测试：删 `format_file`/`list_dir` → 红；删 `tools available:` 行 → 红；`Keep responses concise` 放回种子 → 红；`Chinese for narrative` 放回种子 → 红
- `grep -rn '"coder.md"' src/` 只剩 legacy 两处（`prompts_loader.cpp:291` dispatch、`:349` upgradePromptFile）——前提是 `prompts.h:6-7` 的注释同一提交改掉，否则这里是三处
- 新安装角色列表只剩 assistant.md / compactor.md；老安装里旧三份照常作为普通角色出现在循环中（名字不保留，想删就删文件）
- OSC：`test_osc7501.cpp` 的 "C1 controls" 与 "non-control UTF-8 above the C1 range" 两例已落地（本轮已修）
- `grep -c "web_search" skills/bigbang/config.json` = 1；跑通用题，角色能抓网页
- avenger 跑非商业题（如"给新手村设计怪物 AI 行为树"），文档开头白话摘要、B 段"单人落地路径"而非"创业"
- monica 报告落 `docs/temp/monica-baseline.md`（报告，不落代码）

---

## 9. 落地状态（2026-11-05）

| 项 | 状态 | 提交 |
|---|---|---|
| C1 码点过滤 | 已落地 + 2 例测试 | `fix(osc): filter C1 controls by codepoint` |
| C2 每条必带 app | 已落地（clear 分支挪到 app 之后，空 app 回落 `projv`） | `feat(osc): app on every record…` |
| C3 中断留 error | 已落地（新增 `abandon()`；正常退出仍 clear） | 同上 |
| A 单角色 assistant.md | 已落地（15 工具行、隐藏旧三份、en+zh 03-roles 重写、`test_prompt_seed.cpp` 6 例） | `feat(prompts): one universal assistant role instead of three` |
| B bigbang / avenger 通用化 | 已落地（bigbang 加联网+中性措辞+白话开头；avenger 新增 `research_aud` 摸底轮、按地图派线、B 段由地图推导） | `feat(skills): bigbang and avenger work on any subject` |
| D monica 巡检 | **未跑**：需要真实 API key 与数分钟多智能体运行，且它会改仓库；按本文规则应在分支上人工看 diff | — |
| 验收 | `ctest -C Release` 全绿（282 用例 / 1471 断言）；三个 skill 的 `config.json` 通过 JSON 解析 + 角色 prompt 存在性 + 动作工具可解析校验 | — |

验收里还没做的两件手工事：跑一题通用题目（如"给新手村设计怪物 AI 行为树"）看 avenger 是否真的先摸底、再按地图派线；以及按 §5 在分支上跑一次 monica。

---
