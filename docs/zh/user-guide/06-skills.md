# 06 - 技能

> [English](../../en/user-guide/06-skills.md) | **中文**

> 读完这页你能：跑一个多 Agent 技能、看懂侧边栏、自己写一个技能目录。

## 技能是什么

技能是**数据**，不是代码。放进 `projv_files/skills/` 就能跑，不用重新编译。

```
projv_files/skills/monica/
├── config.json        角色、轮次、收敛规则
├── supervisor.md      每个角色一份系统提示词
├── worker.md
└── tools/emit.json    角色可调用的动作工具
```

## 怎么跑

```
/skill list                # 看装了哪些
/monica D:\ws\myproject    # 用 monica 处理一个项目
/bigbang 加一层 tile 缓存   # 三方圆桌辩论 -> 一份执行文档
```

技能名就是命令名。`Esc` 停止。

## 两个示例

### bigbang

三个角色就一个议题辩论，全部同意后产出一份执行文档。思考模式保持开启 —— 辩论
本身就是产品。

### monica

面向 50~200 文件仓库的项目整理：

1. **supervisor** 先看目录树，按耦合关系切成 6~10 个文件一批，在**同一条回复里**
   把多批同时派出去；
2. **worker_1 ~ worker_10** 并行处理各自的文件：格式化、全角转 ASCII、中文注释
   译成英文，以及只读的死代码审查；
3. supervisor 核对每份汇报，失败的补派，最后写一份汇总报告。

死代码只报告、不删除。worker 没有 shell，格式化走 `format_file`（装了
clang-format 就用真格式化，没装就退化成空白清理）。

### SOP pipeline：avenger -> steve -> musk

三个技能串成一条商业化落地流水线，也正好演示技能编排的两种方式：

1. **avenger** —— 市场调研。Stark 是唯一 driver：一条回复里并行派发 市场 / 用户 /
   开源生态 三条线，可按需点启 技术 / 合规 / 商业模式 三条可选线，对抗性审阅每份
   报告，最后 `DONE` 收口。产出 `docs/sop/avenger_<slug>.md`。
2. **steve** —— 产品设计。**没有 driver**：七个角色全写进剧本（4 设计师 -> PM ->
   QA -> 全体投票），由**引擎**编排。`converge=all_agree` 让"全员同意"由引擎数票
   决定而不是嘴上说；一条异议就把方案退回 PM 修订。产出 `docs/sop/steve_<slug>.md`。
3. **musk** —— 执行。Musk 带着每任务一句 DoD 驱动 dev x test 结对，凭引用出的证据
   裁决，同一任务退回超过三次就升级。产出 `docs/sop/musk_<slug>.md`。

    /avenger 小商家电子发票归档工具
    /steve   小商家电子发票归档工具
    /musk    按 docs\sop\steve_e-invoice-archiving.md 在 D:\ws\sop-test 里做

选型口诀：阵容固定、要机械全票 -> 引擎编排（steve、bigbang）；阵容要临场定 ->
一个 driver（monica、avenger、musk）。

## agent 侧边栏

技能运行时，右侧面板列出每个角色：状态、消息数、当前是否在忙。worker 会同时亮
起来 —— 这就是「并发派活真的生效了」的可视证据。

## 自己写一个

```
/skill skill_maker 一个审查 PR diff 的技能
```

`skill_maker` 编在二进制里。它会写出 `config.json`、提示词和动作工具，然后验证
技能能加载。也可以直接复制 `skills/monica/` 改。

几个要点：

- `tools` 是**技能级**的，所有角色看到同一套能力工具；
- 只有出现在 `round_steps` / `on_converge` 里的角色可以派活（`request_agent`），
  普通 worker 拿不到这个工具；
- 配置里写了非法值会明确报错，写了未知键会在聊天里以警告提示。

---

下一篇 -> [07 pytool](07-pytool.md) | 想改它 -> [技能引擎](../dev-guide/module-map.md)
