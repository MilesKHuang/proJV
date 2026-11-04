# TEMP 设计稿：提示词双角色化 + 技能通用化 + OSC 7501 对齐 + monica 基线

> **TEMP（临时设计稿）** —— 中文、仅供评审，未定稿。
> 不进 `docs/en/` 镜像；定稿后按 `AGENTS.md` 拆成 `docs/en/` + `docs/zh/` 双语正式文档，本文件删除。
> 本文只做设计，不含实现。所有行号来自实际读取；未核实的一律标 `[待证]`。

---

## 0. 这份文档覆盖四件事

| 代号 | 工作项 | 一句话 |
|---|---|---|
| A | 提示词双角色化 | 把四份角色提示词收成 planner + executor，让 proJV 不再只做编码代理 |
| B | 技能通用化 | bigbang / avenger 从"代码调研与方案"扩到"通用调研与方案" |
| C | OSC 7501 对齐 0.4 | 现状已声明 rev 0.4，但有两处与规范不符，必须修 |
| D | monica 巡检 | 用 monica 跑一遍 proJV，产出"实现 MCP 之前的基线" |

A 和 B 是同一件事的两半：A 改主控角色，B 改技能里的角色。C 是独立的合规修补。D 是给后面 MCP 那篇设计立基线。

---

## 1. 现状事实（先看这段，后面所有设计都建立在它上面）

### 1.1 proJV 没有"角色系统"

所谓四个角色，是 `prompts_loader.cpp` 在启动时写到磁盘上的四份纯文本提示词：

- `prompts_loader.cpp:330-333` 的 presets 表：`coder.md` / `designer.md` / `compactor.md` / `analyzer.md`
- 引擎里没有角色表、没有角色→工具映射、没有角色注册表

### 1.2 每个提示词里那行 `tools available:` 就是权限表

```
tools available: read_file, write_file, exec_shell, ...
```

- `agent.h:100` `reloadAllowedTools() { allowedTools_ = parseAvailableTools(systemPrompt_); }`
- `tool_list_parser.cpp` 只认字面量 `tools available:`，按 `,` 切、trim 空格
- **名单外的工具，模型连看都看不见**：`agent.cpp:931` `req.tools = getFilteredToolDefinitions();`，而 `agent.cpp:959-964` 在 `allowedTools_` 非空时按名单过滤

所以"让 planner 不能写代码"不需要新机制，只需要它的文本里不写那三个工具名。

### 1.3 两个静默失效点

1. **空名单 = 不设限**：`agent.cpp:959-964` 的 `if (allowedTools_.empty()) return all;`。新提示词漏写那行，权限不会变窄，而是变成"全放行"，构建绿、测试绿。
2. **默认角色是按字面量找的**：`app_tui.cpp:252-258` 循环找 `"coder.md"`，不是取列表首位。**新建 planner.md 而不改这行，老用户永远落在 coder。**

另外 `app_tui.cpp:259` 的 `promptFiles_[activePromptIndex_]` 在列表为空时越界（既有问题，顺手修）。

### 1.4 现有工具行（四行，逐字核对过）

| 位置 | 角色 | 工具 |
|---|---|---|
| `prompts_loader.cpp:41-42` | coder | read_file, write_file, exec_shell, grep_files, edit_file, file_search, web_search, fetch_url, md_file, update_todo, diagram_tool, run_skill, view_image（13） |
| `prompts_loader.cpp:87` | compactor | md_file, read_file, grep_files（3） |
| `prompts_loader.cpp:190` | analyzer | read_file, grep_files, file_search, md_file, diagram_tool（5） |
| `prompts_loader.cpp:209` | 旧 coder 锚点 | 仅用于样式升级的字符串锚点，**不是活代码** |

**两个孤儿工具**：`format_file`（`format_tool.cpp:130`）和 `list_dir`（`file_tool.cpp:504`）都已注册，但不在上面任何一行里——今天所有角色都用不了它们。

`diagram_tool` 是 pytool 动态注册（`python_tool_manager.cpp` 按 tool.json 注册），`run_skill` / `view_image` 注册在 `app_tui.cpp:59` / `:138`。

---

## 2. 工作项 A：planner + executor

### 2.1 两个角色各拿什么

| 角色 | 工具数 | 名单 |
|---|---|---|
| planner | 11 | read_file, list_dir, grep_files, file_search, web_search, fetch_url, md_file, update_todo, diagram_tool, run_skill, view_image |
| executor | 15 | planner 全部 + write_file, edit_file, exec_shell, format_file |

差集就是那四个"动手"的工具 —— 这就是"计划 / 执行"的边界，其余完全一样。

**顺手救活两个孤儿**：`list_dir` 进 planner（读目录本来就是计划的一部分），`format_file` 进 executor（执行的一部分）。这是本轮唯一一次会重写工具行的机会，下一版没人会为两个名字再来一次。

### 2.2 改哪些文件

| 文件 | 动作 |
|---|---|
| `prompts_loader.cpp` | presets 表换成 `planner.md` / `executor.md`；新增两段种子文本；`upgradeSeededPrompt`（:290-298）补两个分支走 `upgradeStyleText`（**不补则 styles 测试首轮红 6 条**）；`upgradePromptFile`（:349-352）补两行 |
| `prompts.h` | 加 `defaultPromptSeed(name)`、`defaultPromptIndex(list)` 两条声明 |
| `app_tui.cpp` | `:252-258` 的循环换成 `defaultPromptIndex`；`:250-251` 的 erase 追加三个旧角色名；`:259` 加空表守卫 |
| `test_prompt_style.cpp` | files[] 追加两个新名字；planner / executor 各一条集合相等断言；`planner != executor` |

### 2.3 硬约束

- **不删用户文件**：`coder.md` / `designer.md` / `analyzer.md` 留在磁盘，只从 F4 列表过滤掉。想要回来自行改名。
- **不做等值迁移**：不为"删旧种子"养两个 legacy 常量和一段不可回滚的删除逻辑。
- **断言必须成对**：只测"含"会放行一个照抄 coder 的残废种子。

### 2.4 已知缺口（写清，别当 bug 报）

- **planner 只能写 `.md`**：`md_file` 强制后缀 + 8KB 上限，所以"把计划存成 plan.json"做不到。
- **角色边界只存在于提示词文本**：没有代码强制。抓得住"种子被改坏"，抓不住"有人用一句话说服模型越界"。

---

## 3. 工作项 B：bigbang / avenger 通用化

### 3.1 这里有个真问题，不是措辞问题

bigbang 的三份角色提示词里有条硬规矩：

> Any claim about the codebase (file path, signature, line number, behavior) MUST come from a file you read or grepped THIS round.

通用调研（市场、竞品、方案对比）**没有文件路径可引**。照抄这条，Sheldon 会一路否决到打满 `max_rounds: 8`。

**改法**：把"必须来自本轮读过的文件"扩成"必须来自本轮读过的来源：文件路径，或 URL/检索结果"。同时保留后半句（未核实必须标 `[assumed]`）——那才是这条规矩的价值。

### 3.2 工具闸门已验证：加名字就生效

`skills/*/config.json` 的 `"tools"` 数组是**真闸门**，不是死配置：

- `sub_agent.cpp:186-189` 执行时逐个比对白名单，不在名单返回 `tool '<x>' not allowed`
- `skill_runner.cpp:393-394` 按同一名单把 schema 注册给模型

所以 bigbang 现在只有 `read_file, grep_files, file_search`（零联网能力），要做通用调研必须加 `web_search` / `fetch_url`。这一条是 grep 出来的事实，不是推测。

### 3.3 改哪些文件

| 文件 | 动作 |
|---|---|
| `skills/bigbang/config.json` | `tools` 加 `web_search`、`fetch_url`；`description` 去掉"engineering" |
| `skills/bigbang/{sheldon,penny,leonard}.md` | 证据规则从"文件"扩到"文件或 URL"；把"design document""codebase"这类词改成通用说法 |
| `skills/bigbang/tools/write_bigbang_doc.json` | description 里的"execution document"改通用 |
| `skills/avenger/config.json` | 同上加 web 工具 |
| `skills/avenger/research_*.md`、`stark.md` | 同 B 的证据规则与措辞 |
| `skills/*/README.md` | 说明这两个技能不再只面向代码 |

### 3.4 风险

- 加 web 工具后单个 worker 挂起会拖长整轮；`max_rounds: 8` 的墙钟时间可能翻倍。skill 阶段目前没有 per-tool 超时。
- 通用题目下"引用来源"的质量取决于检索结果，`[assumed]` 标记会变多——这是对的，不是退步。

---

## 4. 工作项 C：OSC 7501 对齐最新版

### 4.1 先纠正一个前提

`terminal_status.h:2` 写着 "rev 0.4"，而规范当前的**最新版就是 0.4（2026-10-10）**。所以这不是"升级到新版本"，是**逐条对齐 0.4 并修掉不合规处**。

规范地址（`terminal_status.h` 里记的那一条）：`https://www.superlogical.com/rex/docs/build/program-status`
版本历史：0.1（09-28）→ 0.2（10-06）→ 0.3（10-07）→ **0.4（10-10，当前最新）**。

### 4.2 核对结果

| # | 0.4 的要求 | 现状 | 判定 |
|---|---|---|---|
| C1 | 解码后的 `msg`/`title` **禁止** U+0000–U+001F、U+007F、**U+0080–U+009F** | `terminal_status.cpp:104` 只判 `c < 0x20 \|\| c == 0x7f`（逐字节） | **不合规，必修** |
| C2 | 记录是**整体替换**，每条报文都必须重复 `app` | `:167` `if (!app.empty()) body += ":app=" ...` | 有隐患，建议改为必填 |
| C3 | 完成即退出时应留 `done`/`error` 记录 | `shutdown()` 发的是 `state=clear`（`:230`） | 设计取舍，需拍板 |
| C4 | 用户中断/取消时上报 `idle` | `[待证]` 需核 `Agent::cancel` 路径 | 待核 |
| C5 | `ST` 可以是 `ESC \` 或 `BEL` | 只用 `ESC \`（`kEnd`） | 合规（可保留） |
| C6 | 特性探测可选，可不等回复直接上报 | 从不发 `OSC 7501 ; ? ST` | 合规 |
| C7 | 上限：4096 / key 16 / msg 2732·2048 / title 256·192 / app 32 / id 128 | 常量逐一对应 | 合规 |

### 4.3 C1 为什么是真 bug

UTF-8 里 U+0080–U+009F 编码成两字节 `C2 80`..`C2 9F`。逐字节判 `c < 0x20 || c == 0x7f` 两个字节都不命中，于是这些字符原样进 base64。

规范的处置是**整条报文作废**：

> A report is discarded whole if it exceeds one of the limits, if its base64 does not decode, or if decoded text contains a control character.

也就是：宿主看到 C1 控制字符 → **整条丢掉** → proJV 以为上报了，宿主那边什么都没有。用户看到的是"状态栏不更新"，而且**没有任何日志**。严格实现（Rex 自己那套）会这样处理，宽松实现不会——正好是最难复现的一类 bug。

**修法**：`sanitizeControl` 改成按码点判定（先按 UTF-8 解码，替换所有 C0/C1/DEL，再编码），或者最小改法——额外挡住 `0xC2` 后面跟 `0x80`..`0x9F` 的两字节序列。前者正确，后者够用。

### 4.4 C3 需要拍板

规范原文：

> A program that exits as soon as it finishes SHOULD report `done` or `error` right before exiting, so a record is left behind for the user to find.

proJV 是长驻 TUI，用户关窗口 = 不再需要状态 → `clear` 合理。但两种场景应区分：

- 用户主动关（`q` / 关标签页）→ `clear`，现状正确
- 崩溃 / 被 Ctrl-C → 应留 `error`，否则宿主里那条记录会一直显示"working"

**决定**：正常退出保持 `clear`；信号/异常路径补一条 `error`。这是本轮唯一需要改 `shutdown()` 语义的地方。

---

## 5. 工作项 D：monica 巡检，作为 MCP 的新基线

### 5.1 monica 是什么（读配置得到的）

`skills/monica/config.json`：**1 个 supervisor + 10 个 worker**，`max_parallel: 16`、`max_rounds: 10`、`converge: none`、`stop_when` supervisor 回 `DONE`。

它做的事：格式化、全角转半角、把中文注释翻成英文、**只报告不删除**的死代码审计。工具集含 `write_file` / `edit_file` / `format_file` —— **它会改仓库**。

设计规模是 50–200 文件；proJV 的 `src` + `tests` 大致在这个区间，正好是它的目标场景。

### 5.2 为什么把它当成 MCP 的新基线

MCP 那篇设计里有大量 `[assumed]`（尤其协议理解），而它的所有结论都建立在"当前代码是这样"之上。跑一次 monica 能得到：

1. **格式与标点统一**：MCP 落地时那次 diff 不会混进格式噪声
2. **中文注释清理**：`AGENTS.md` 要求源码只写英文；现状有残留
3. **死代码清单**：MCP 要往 `tools/` 加东西，先知道哪些是死的
4. **一份逐文件的现状报告**：这比"MCP 设计稿里我们猜的"更可靠，直接作为实施起点

### 5.3 怎么跑（必须遵守）

- **在分支上跑**，不要在 `main` 上跑。它有写权限。
- 跑完先看 `git diff --stat` 和死代码清单，**不要盲merge**。
- 它的死代码结论是"报告"，不是"删除"。任何删除都要人来做。
- 顺序：放在 A、B 之后。否则 A/B 改完的文件又要被它再格式一遍。

---

## 6. 顺序与依赖

```
C（OSC 对齐，半天，独立可发）
  ↓
A（双角色）  →  B（技能通用化）
  ↓
D（monica 巡检，在分支上）
  ↓
MCP 实施（见另一篇）
```

两条硬依赖：

1. **A 必须早于 MCP**：MCP 的 `roles = ["..."]` 是按提示词文件名做 key 的，而今天没有 planner/executor 这两个文件。A 不做，MCP 的示例就只能写 `roles = ["coder"]`。
2. **D 必须在 A/B 之后、MCP 之前**：否则那份"基线"记录的是即将被改掉的代码。

---

## 7. 风险表

| 风险 | 触发 | 严重度 | 立刻 | 长期 |
|---|---|---|---|---|
| 新提示词漏写 `tools available:` → 权限变成全放行 | 手写种子 | 高 | 集合相等断言 | 让 `parseAvailableTools` 空结果直接报错而不是放行 |
| 忘了改 `app_tui.cpp:252-258` 的字面量 → 新角色永不生效 | 只加文件不改代码 | 高 | 启动日志打印当前角色与工具数 | 角色默认值进配置 |
| `upgradeSeededPrompt` 不加分支 → styles 测试红 6 条 | 只加文件名 | 中 | 同提交改两个分支 | dispatch 改表驱动 |
| `promptFiles_` 为空 → `:259` 越界 | 用户清空 prompts 目录 | 中 | 加空表守卫 | 索引统一走 `defaultPromptIndex` |
| OSC C1 控制字符放行 → 整条报文被宿主丢弃 | 消息里带 U+0080–U+009F | 中 | 按码点过滤 | 加一例含 C1 字符的编码测试 |
| monica 在 main 上跑并提交 | 直接执行 | 高 | 分支 + 人工看 diff | 技能加"只读模式"开关 |
| 新注册工具无人可用（历史上的 format_file / list_dir） | 加工具忘了改工具行 | 中 | 本轮已补 | 加一条"注册表 ⊆ 某个角色工具行"的检查 |
| bigbang 加 web 工具后单轮墙钟时间翻倍 | 通用调研题目 | 低 | 文档标注 | skill 阶段加 per-tool 超时 |

---

## 8. 验收

- `ctest -C Release --output-on-failure` 全绿；新增断言 ≥ 4 条（planner 集合、executor 集合、两者不等、`defaultPromptIndex`）。
- 手改测试：把 planner 的工具行换成 executor 的那行 → 必须红；删掉 executor 种子里的 `tools available:` 行 → 必须红。
- `grep -rn '"coder.md"' src/` 只剩 `prompts_loader.cpp` 里两处 legacy 路径（dispatch `:291`、`upgradePromptFile` `:349`）。
- F4 列表只剩 planner / executor；旧文件在磁盘上仍存在。
- OSC：`state=working:msg=` 里塞一个 C1 字符，编码结果不得包含 `C2 80`..`C2 9F`。
- `grep -c "web_search" skills/bigbang/config.json` 为 1；跑一次 bigbang 通用题目，角色能真的抓网页。
- monica 报告落进 `docs/temp/monica-baseline.md`（报告，不落代码）。
