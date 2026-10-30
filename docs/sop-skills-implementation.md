# SOP 三 Skill 实现总结（avenger / steve / musk）

提交：`abbf1a1`（`feat/sop-skills`，37 文件 / 1360 行）　引擎：v0.6.3 **零改动**

## 1. 这一轮做了什么

加了三个**纯数据**的多智能体 skill，串成"调研 → 设计 → 执行"一条流水线。没有改一行 C++：它们用到的能力（动态派发、`stop_when`、`all_agree` 投票、`max_tool_iters`、`submit_reserve`、`file_requests`）v0.6.3 全部已有。

| 交付 | 路径 | 内容 |
|---|---|---|
| 技能 | `skills/avenger/` | `config.json` + `stark.md` + 6 条研究线提示词 + 3 个 verb 工具 |
| 技能 | `skills/steve/` | `config.json` + 7 个角色提示词 + 3 个 verb 工具 |
| 技能 | `skills/musk/` | `config.json` + `musk/dev/test` + 3 个 verb 工具 |
| 测试 | `tests/unit/test_skill_sop.cpp` | 6 用例 / 148 断言，脚本化不联网 |
| 文档 | `docs/{en,zh}/user-guide/06-skills.md`、`skills/README.md` | 双语同步：pipeline 说明 + 两种编排模式 |

安装方式不变：把 `skills/<name>/` 复制进 `projv_files/skills/` 即可（`/skill list` 能看到）。

## 2. 核心设计决策：两种编排模式，按"阵容是否固定"选

| 模式 | 谁在编排 | 机制 | 收敛判据 | 用在哪 |
|---|---|---|---|---|
| 剧本模式 | 引擎（`executeSteps`） | 角色和步骤写死在 `round_steps`；`parallel` 给并发；`shape=vote` 收票 | `converge=all_agree`（机械数票） | **steve**、bigbang、boss_forge |
| 现场模式 | 一个 agent（唯一 driver） | `dynamic_dispatch` + `request_agent`，运行中决定派谁 | `stop_when`（driver 打 DONE） | **avenger**、**musk**、monica |

为什么 steve 不用 driver：引擎规则是"在动作脚本里露过面 = driver，driver 不能再被派活"。让 Steve 现场派活给 4 个设计师、又让设计师在投票轮露面，Steve 一 `request_agent` 就必然报错，而且 `votes_` 永远为空 → `all_agree` 永不触发 → 静默跑满轮数。设计阵容固定且必须齐整，所以把编排交给引擎：串行步骤天然保证 PM 看到的是做完的设计，比"靠 driver 记得等"更硬。

## 3. 三个 skill 的形态

- **avenger**：Stark 是唯一 driver，一条回复里并行派核心 3 线（市场/用户/开源），可按需点启技术/合规/商业模式（不点即休眠，零成本）；对每份报告做三问对抗审阅（是否回答生意问题 / 有没有没看到的点 / 事实之间有没有潜在关联），有盲区就定向补派；每条线写自己的文件，收口时合并。
- **steve**：7 个角色全在剧本里：Steve 定核心价值 → 4 设计师并行 → PM 统整 → QA 验收标准 → 全员投票。一条异议就把方案退回 PM 修订（修订步骤用 `when: round>1`，只在上一轮没全绿时才跑），全绿才收敛。
- **musk**：Musk 读设计方案拆看板，每任务一句可验收的 DoD，dev 实现 → 独立 test 对照 DoD 复核 → 按判据裁决；同任务退回超过两次就**重开成更小的任务**，不无限循环。dev/test 只回 ≤15 行摘要 + 证据，细节留文件。

## 4. 实现中踩到的三个坑（都已在配置层解决）

1. **`on_converge` 只发 TUI 事件，不落盘**。这是最致命的：文档里写着"Musk 读 steve 文档"，但那个文件根本不存在。修法：收尾步骤先 `md_file` 分块写盘（>8KB 用 `append`），再调 verb 提交。三个 skill 都已改，avenger 额外要求文件**以不超过 60 行的一页摘要开头**（结论 / 切入点 / 自研还是复用 / 未证实的风险 / 最便宜的下一步），并读回文件尾部自证没被截断。
2. **模板语法**：`{{X.current}}` / `{{X.prev}}`，没有 `slot.` 前缀；未知 key 填空字符串且**不报警**，写错只会"静默无内容"。另外 `{{X.prev}}` 是上一轮末的快照——收敛那一轮不会更新它，所以收尾步骤要取"最后一句判断"必须用 `.prev`。
3. **`tool_mode` 在 `dynamic_dispatch` 下被强制成 `auto`**：driver 的提示词必须明写"最后一定要调用 X 提交"，否则模型可能只在正文里说完了事。

## 5. 验证方式（可重复）

```
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure          # 100% passed
build\tests\Release\proJV_tests.exe -tc="sop:*"                # 6 cases / 148 assertions
```

单测断言的是"配置写错就红"的那部分：三份 config 解析零 warning（未知键只 warn，靠它抓 typo）；每个 agent 的提示词文件、每个动作的 verb 工具文件都存在且能解析；steve 确实是剧本（`dynamic_dispatch=false`、7 个投票动作、修订步骤在投票之前）；steve 全同意时第 1 轮收敛、有异议时第 2 轮修订后再收敛；avenger 一条回复并行派 3 条线且可选线保持休眠；musk 的 dev 先于 test、未派发的 agent 零调用。

三个 skill 另外各做过一次真机运行（产出物已按需清理，不入库），确认了动态派发、按需点启、`stop_when` 精确命中、`all_agree` 机械收敛、dev×test 拮抗五条机制真的生效。

## 6. 已知限制

- **avenger 的"摘要页"要求是配置层强制的，还没有用一次真机运行复验**（第一次运行暴露了不落盘的问题，修完只验证过 steve 与 musk）。
- **musk 链路里 `exec_shell` 没有审批门**（审批对话框只挂在主 Agent 上），现在靠三份提示词的硬约束（只跑构建/测试、只在项目目录内、不 push、不删目录、不装依赖）。要硬保证需要在引擎侧加命令白名单。
- **技能产物写在 workspace 的 `docs/sop/`**，不进 proJV 仓库；`docs/archive/` 被 `.gitignore` 排除（local planning archive）。
- 没做的（明确的下一版才做）：`find --amount`、`--json`、覆盖率三分类、avenger 的 `dispatch_from`（若将来要 avenger 也强制全票，才需要动引擎）。

## 7. 怎么用

```
/skill list
/avenger <一个市场题目>          # 产出 docs/sop/avenger-<slug>.md（摘要页 + 各线报告）
/steve   <同一题目，或指向 avenger 的文件>   # 产出 docs/sop/steve-<slug>.md
/musk    <指向 steve 的文件 + 项目目录>      # 产出 docs/sop/musk-<slug>.md（看板 + 执行记录）
```

三者可独立运行，也可串行；串行时由主 Agent 依次触发并把上游文件路径写进下游的 topic。
