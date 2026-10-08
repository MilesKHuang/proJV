# Multi-Agent UX 优化执行文档

> 目标：优化 proJV 多 Agent（`/skill bigbang`、`/skill monica`）的使用体验与流程。
> 关联代码：`src/core/sub_agent.*`、`src/core/skill_runner.*`、`src/core/skills_builtin.*`、`src/core/prompts_loader.cpp`、`src/core/prompts.h`、`src/tui/app_tui.*`、`src/tui/main_tui.cpp`、`skills/monica/`。
> 已定决策：命名 = A（目录 `bigbang`）；安装 = a（纯可插拔）。

---

## 0. 关键不变量（先读，避免踩坑）

- **技能 = 文件夹。** `SkillRunner::run(skillName)` 读 `skillsDir_/<skillName>/config.json`；随后 `loadAgents()` 用 **config.json 里的 `name`** 去拼 `skillsDir_/<name>/tools` 与 `skillsDir_/<name>/<promptFile>`。
- 因此必须满足：**`run()` 的参数 == 文件夹名 == config.json 的 `name`**，三者完全一致。否则 tools/ 与 prompt 找不到，agent 无工具、system prompt 为空。
- 本方案取值：文件夹 `bigbang`、config.json `name` = `bigbang`、测试 `run("bigbang")`、命令 `/skill bigbang <topic>`。
- **安装模型（决策 a）**：程序不再为 bigbang 特例种子化。开箱即用**不含** bigbang；安装 = 手动拷贝 `skills/bigbang/` 到 `projv_files/skills/`，卸载 = 删除该目录。

---

## 1. 需求总览与优先级

| # | 需求 | 方案 | 工作量 | 类型 |
|---|------|------|--------|------|
| 1 | Sidebar 显示每个 Agent 状态与上下文占用 | 引擎快照接口 + TUI 面板 | 大 | 引擎 + TUI |
| 2 | 统一命令口径为 `/skill <name>`（monica / bigbang） | 删除 `/bigbang` 分支 | 小 | 命令路由 |
| 3 | monica 每轮一句话回报 + 显示内容拆分 | 改 supervisor prompt（零引擎改动） | 中 | prompt/DSL 层 |
| 4 | 轮数按 skill 配置 | `config.json` 的 `max_rounds` 字段（不改代码） | 无 | 配置 |
| 5 | bigbang 独立 skill 文件夹（可插拔，无特例） | 与 monica 同构的 `skills/bigbang/`；删内嵌常量与特例种子化 | 中 | 结构重构 |

**建议推进顺序**：3 → 2+5（联动重构）→ 1。

---

## 2. 执行方案明细

### 2.1 需求 3：monica 每轮一句话回报（零引擎改动）

**目标**：supervisor 每轮输出一行摘要回报；每轮结束说明改了啥。

**改动**：改 `skills/monica/supervisor.md` 的 prompt 约束：
- 每轮 `emit(summary)` 输出**一行**摘要：`Round {n}/{max}: 格式化 2 文件 / 翻译 4 注释 / 扫描 30 文件，无死代码`
- 细节与逐文件清单通过 `write_file` / `md_file` 落盘，不进泡泡。
- 最终 `DONE` 时输出一句话总结 + 文档路径。

**效果**：现有黑板块（supervisor slot）文本自然变短，TUI 显示即改即生效。

### 2.2 需求 2 + 5：统一命令口径 + bigbang 独立 skill 文件夹（可插拔，无特例）

**原则**：skill = 独立文件夹，可插拔。bigbang 与 monica 一视同仁——没有内嵌常量、没有"默认生成"、没有双路径查找、没有命令别名。

**目标**：
- 命令统一为 `/skill <name>`：`/skill bigbang`、`/skill monica`、`/skill list`。
- `bigbang` 成为与 monica 完全同构的独立 skill 文件夹；代码中不内嵌任何技能内容（config / prompt / tool 定义全部为文件）。
- 安装与 monica 一致：拷贝 `skills/<name>/` 到运行时的 `projv_files/skills/` 即用；删除即卸载。

**改动清单**：

1. **创建 repo 内 `skills/bigbang/` 文件夹**（与 `skills/monica/` 完全同构）：
   - `config.json`：搬迁 `BIGBANG_CONFIG_JSON`（`skill_runner.cpp:43-100`）。**唯一字段改动：`"name"` 由 `bigbang_debate` 改为 `bigbang`**（见第 0 节不变量）；其余（含 `max_rounds: 8`、`round_steps`、`on_converge`）逐字节一致。
   - `sheldon.md` / `penny.md` / `leonard.md`：逐字节搬迁 `PROMPT_DEFAULT_SHELDON/PENNY/LEONARD`（`prompts_loader.cpp:241-398`）。
   - `tools/bigbang_turn.json` / `tools/bigbang_vote.json` / `tools/write_bigbang_doc.json`：逐字节搬迁 `TOOL_TURN/TOOL_VOTE/TOOL_DOC_JSON`（`skill_runner.cpp:36-38`）。
   - **保持 `display_name` 仍为 `Sheldon` / `Penny` / `Leonard`**：`bubble_model.cpp:131` 与 `chat_view.cpp:51` 硬编码这三个名字做气泡 speaker 与配色键，改名会导致 TUI 着色失效。

2. **删除 bigbang 特例种子化（3 个文件）**：
   - `skill_runner.cpp`：删 `BIGBANG_CONFIG_JSON` + `TOOL_*_JSON`（`:35-100`）；`ensureDefaultSkills` 里删 bigbang 分支（`:187-197`），**仅保留 `seedBuiltinSkills(skillsDir)`**。
   - `prompts_loader.cpp`：删 `PROMPT_DEFAULT_SHELDON/PENNY/LEONARD`（`:241-398`）与 `bigbangPromptsDir` / `loadBigbangPrompt`（`:400-415`）。
   - `prompts.h`：删 `loadBigbangPrompt` 声明（`:14-16`）。
   - 措辞澄清：`ensureDefaultSkills` 之后仍会（经 `seedBuiltinSkills`）为 **`skill_maker`** 种子化，这是引擎自举技能，与 bigbang 无关。**不再为 bigbang 生成任何文件**；bigbang 仅以 repo 内 `skills/bigbang/` 文件夹为唯一载体。

3. **测试适配（必做，否则 `test_skill_runner.cpp` 4 个用例全挂）**：
   - `tests/unit/test_skill_runner.cpp` 4 个用例：删掉开头的 `ensureDefaultSkills(getProjvDir() + "/skills")`；构造改为 `SkillRunner sr(testConfig(), nullptr, cbs, "", PROJV_EXAMPLE_SKILLS_DIR)`；并把 4 处 `run("bigbang_debate", "T")` 改为 `run("bigbang", "T")`。
   - `PROJV_EXAMPLE_SKILLS_DIR` 已由 `tests/CMakeLists.txt:50` 定义为 `<repo>/skills`，`test_skill_monica.cpp` 已在用，**零新增**。
   - golden 断言（消息模板、`lastRounds()==8`、`voteSummary` 格式）保证 config.json 的 `round_steps` / `max_rounds` 与 3 个 `.md` 内容与旧常量一致；config.json 的 `name` 改动**不影响**这些断言。
   - 备选（若不想依赖 repo 相对路径）：把 `skills/bigbang/` 拷到测试临时目录，再把该目录当 `skillsDir` 传入。

4. **删除 `/bigbang` 命令分支**（`app_tui.cpp:215-231`），统一由 `/skill` 路由（`app_tui.cpp:233-260`，零新增）。
   - 用户输入 `/skill bigbang <topic>` 即触发辩论；`startSkill` 的持久化前缀已是 `/skill `。

5. **文档 / README 同步**（可延后到版本发布统一改，但需登记）：
   - `README.md` / `README_zh.md`：`/bigbang` → `/skill bigbang`；补 `skills/` 安装说明（与 monica 一致）。
   - `BIG_BANG_DEBATE.md` / `BIG_BANG_DEBATE_FIX.md`：正文与示例中的 `/bigbang`、`projv_files/prompts/bigbang/` 路径全部过时，标注为历史设计文档或改写。
   - `SKILL_ENGINE.md`：§5 / §8 / §11.6 明确写 "bigbang = built-in skill #1, seeded" 与 `/bigbang`，与新模型冲突，需改为 "example skill in repo `skills/bigbang/`，手动安装"。

> 顺带清理：`loadBigbangPrompt` 原逻辑先读 `{promptsDir}/bigbang/<role>.md`，但种子化写入的是 `skills/bigbang_debate/`，该分支永远读不到、纯死路径；删除即消除。

**注意**：技能即文件夹，没有"默认生成/双路径查找/命令别名"。未安装时 `/skill list` 不列出该技能；安装 = 拷贝 `skills/<name>/` 进入 `projv_files/skills/`。

**开箱体验变化（决策 a 的预期代价）**：全新安装不再自带 bigbang，`/bigbang` 与 `/skill bigbang` 在未安装前不可用。这与 monica 完全一致；分发时需提示用户安装。

**迁移（老用户）**：
1. 删除旧目录 `projv_files/skills/bigbang_debate/`（种子化不覆盖已存在文件，会与旧入口并存）。
2. 拷贝 repo 的 `skills/bigbang/` 到 `projv_files/skills/`。
3. 之后使用 `/skill bigbang <topic>`。

### 2.3 需求 4：轮数保留 JSON 配置（不改代码）

- `max_rounds` 已是 `config.json` 顶层字段，改 JSON 即可（`SkillConfig::loadFromJson` 已读取）。
- bigbang 用不满轮数是收敛机制提前停（`all_agree` / `loop_detect`），属预期行为，不做动态参数。
- monica 的轮数上限由 `max_rounds`（12）与 `stop_when: {slot: supervisor, equals: DONE}` 共同控制。

### 2.4 需求 1：Sidebar Agent 状态面板（大项，需并发方案）

**目标**：TODO 面板下方（`main_tui.cpp:342` 右侧 dock）新增 Agent 状态区，显示：
- 每个 Agent 的 id / display_name / 当前动作（最后一条 narration 摘要）
- 上下文占用（消息数）

**改造路径**：

1. `SubAgent` 增加字段与只读 getter（`sub_agent.h/.cpp`）：
   - 新增成员：`std::string lastNarration_`（`turn()` 内每次得到文本时更新）、`std::atomic<bool> busy_{false}`。
   - `int messageCount() const { return (int)session_.messageCount(); }`（`Session` 已有内部锁，安全）。
   - `std::string lastNarration() const;` **按值返回**（跨线程读，不可返回引用）。
   - `bool busy() const { return busy_.load(); }`。
   - `turn()` 入口置 `busy_=true`，退出（含所有 return 路径）置 `false`；对 `lastNarration_` 的写用 per-agent `std::mutex` 或改为原子字符串替换。

2. `SkillRunner` 增加并发安全快照（`skill_runner.h/.cpp`）：
   - `struct AgentSnapshot { std::string id, displayName, status; int msgCount; bool busy; };`
   - `std::vector<AgentSnapshot> agentStatuses() const;`
   - 保护：新增 `mutable std::mutex snapMutex_`，并让 `loadAgents()` 写 `agents_` / `displayNames_` 时持同一把锁（或填完后置 `agentsReady_` 标志，读取端仅在 ready 后读）。
   - `status` 取该 agent 的 `lastNarration()`（截断到一行）；快照**只拷贝数据，不返回引用**。

3. `TuiApp` 转发（`app_tui.h/.cpp`）：`std::vector<SkillRunner::AgentSnapshot> skillAgentStatuses() const;` —— 锁 `skillMutex_`，判空 `skillRunner_`。

4. `main_tui.cpp`：在 TODO 面板下渲染固定高度的 agent 列表，`vbox({ todoPanel, separator, agentPanel })`（当前为 `hbox({chatColumn, separator, todoPanel})`，需把右侧 dock 改为纵向 vbox）。

**并发注意**：
- 每个 agent 只在 `turn()` 内被子线程访问；快照读取与写入需同步（map 用 `snapMutex_`，字段用 `atomic` / per-agent 锁）。
- FTXUI 每帧调用 renderer，快照读取必须**非阻塞、按值拷贝**。
- `agents_` map 的生命周期：`loadAgents()` 填充期与 TUI 读取期互斥。

---

## 3. 验收清单

| # | 验收项 | 对应需求 |
|---|--------|---------|
| 1 | Sidebar 显示全部 agent 的 id/状态/消息数，skill 运行时实时刷新 | 1 |
| 2 | `/skill bigbang <topic>` 触发辩论；`/skill monica <root>` 触发清理；`/bigbang` 已移除 | 2 |
| 3 | monica 每轮输出一行摘要回报，最终一句话总结 + 文档路径 | 3 |
| 4 | 修改 skill 的 `config.json` 的 `max_rounds` 生效 | 4 |
| 5 | 拷贝 `skills/bigbang/` 到 `projv_files/skills/` 即装好、可运行；删除即卸载（与 monica 一致） | 5 |
| 6 | 代码中无 bigbang 内容常量与特例种子化逻辑（config/prompt/tool JSON 全部为文件） | 5 |
| 7 | `/skill list` 安装后同时列出 bigbang 与 monica | 2+5 |
| 8 | `tests/unit/test_skill_*` 全部通过（bigbang 收敛契约不回归） | 1-5 |

---

## 4. 风险与边界

- **命名三一致**：`run()` 参数、文件夹名、config.json `name` 必须相同（第 0 节）；这是本次最容易踩的坑，改 config 的 `name` 是硬要求。
- **TUI 硬编码角色名**：`bubble_model.cpp` / `chat_view.cpp` 依赖 `Sheldon/Penny/Leonard`，display_name 不可改。
- **并发**：需求 1 是唯一动引擎并发模型的大项。`SkillRunner` 目前无锁（并行 steps 各写本地 `result[]`，合并由 runner 线程顺序执行）；加快照接口需保持这一不变量，快照只读、加独立 `snapMutex_`。
- **引擎语义盲**：需求 3 只改 prompt 不改引擎，保持"引擎不含 workflow 特定词"的边界。
- **安装约定**：技能以文件夹为唯一载体，运行时从 `projv_files/skills/` 加载；分发时需一并携带 `skills/<name>/`（或提示用户拷贝安装）。开箱无 bigbang 是决策 a 的既定行为。
- **回归**：改动后跑 `tests/unit/test_skill_runner.cpp`（含 skill_maker / swarm / engine / monica），确保 bigbang 收敛契约（unanimous / round cap / loop-detect）不回归。
- **行号**：本文行号为当前快照参考，改动后会漂移，以符号名为准。
