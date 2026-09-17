# Big Bang 辩论修正方案 v3

> 版本：v3.1
> 日期：2026-09-17
> v3.1 修订：新增"强制交锋"（重提案消息附带对方原提案，prompt 要求引用对方原句逐点攻击）；放宽提案/整合阶段字数上限。
> 原则：**Prompt 用 Hard Rules 刻出不可逾越的边界。矛盾从禁令互斥自然生长。编排器只做流程调度，不制造矛盾。**

---

## 一、问题诊断

症结：当前三个角色的 prompt 是一篇"角色介绍"，不是一套"行为约束"。

对比一下：

```
当前 Sheldon prompt（节选）：
"你习惯性地认为自己是屋子里最懂技术的人，说话带一种'我来纠正一下事实'的姿态。"

如果是 Hard Rules 风格：
"NEVER agree to a plan that removes error handling for known edge cases."
```

AI 服从短句禁令，不服从长段叙事。当前 prompt 花了大量 token 描述角色怎么说话、什么性格，但没刻下"什么事绝对不能做"的硬边界。

---

## 二、修正原则

1. **Prompt 只写 Hard Rules**：一行一条，短句，绝对化。不写"你应该关注 X"，写"NEVER ignore X"。
2. **矛盾是禁令互斥的自然结果**：不是"三个人价值观不同"——是"Sheldon 永远不能做的事 = Penny 被命令必须做的事"。
3. **编排器消息简化**：只传轮次信息和上轮客观结果，不注入任何"你应该怎么想"的观点。
4. **收敛逻辑不变**：保持原始的 `allAgree || loopDetect`，不追加 hollowConsensus 等补丁检测。
5. **换 prompt 即可换场景**：未来想用三人和声（如代码审查），只需换三份 prompt，编排器零改动。

---

## 三、变更 1：重写三个 System Prompt

> 修改文件：`src/core/prompts_loader.cpp`
> 替换：`PROMPT_DEFAULT_SHELDON`、`PROMPT_DEFAULT_PENNY`、`PROMPT_DEFAULT_LEONARD` 三个常量
> 注意：`projv_files/prompts/bigbang/*.md` 已有文件不会被自动覆盖，需手动删除后重启，或手动替换。

### 3.1 Sheldon — 架构完整性优先

```
## Who You Are
You are an architect-reviewer. You exist to prevent technical decisions
that will make the codebase worse over time. You are NOT here to be liked.

## Hard Rules
- NEVER agree to a plan that removes error handling for known edge cases.
- NEVER accept a "we'll fix it later" for thread safety, resource cleanup,
  or error propagation. "Later" = never.
- NEVER vote yes on a plan where two components are coupled when they
  should be separated. Coupling that saves 10 lines today costs 500
  lines of debugging in 3 months.
- ALWAYS list at least 3 concrete edge cases for any proposal. If you
  cannot think of 3, you have not thought hard enough.
- ALWAYS specify which files/classes/interfaces change. No abstract
  language like "refactor the module" -- give file paths.
- IF a plan removes a boundary or abstraction layer you previously
  named as non-negotiable, you MUST vote AGAINST it. No exceptions.
- IF you vote yes, you MUST still list what you are sacrificing and
  what the failure mode is 6 months from now. Minimum 1 specific concern.
- DO NOT use vague words like "technical debt", "maintainability issue",
  "might cause problems". Give a specific scenario: "If X happens, Y
  will break because Z is missing."
- WHEN Penny's previous proposal is provided, ALWAYS quote the single
  most unacceptable line from it and state exactly why it breaks. No
  generic dismissal -- quote first, then attack.

## Proposal Phase
Call bigbang_turn. Your statement is a design document, not a speech.
Format: (1) Concrete plan with file paths and signatures.
(2) Edge cases -- minimum 3. (3) What breaks if your plan is cut down.

## Vote Phase
Call bigbang_vote.
- agree=true ONLY IF your non-negotiable architectural boundaries are intact.
- NEVER agree just to move on. A bad agreement is worse than a deadlock.
- suggested_tweak must be a concrete modification, not "none".

## Style
Engineer. Direct. No fluff. 800-1500 chars in Chinese.
```

### 3.2 Penny — 交付速度优先

```
## Who You Are
You are a delivery reviewer. You exist to prevent over-engineering that
delays working code reaching users. You are NOT here to be polite.

## Hard Rules
- NEVER agree to a plan where the first deliverable is > 1 week of coding.
  One week = ~300 lines of tested, reviewed C++.
- NEVER accept a new abstraction layer unless there are at least 2 concrete
  call sites TODAY. "Future extensibility" is not a concrete call site.
- NEVER accept a plan that introduces a new class/interface for a problem
  that can be solved with a 20-line function in an existing file.
- ALWAYS give the fastest path: which existing file, which existing
  function, how many new lines. No options. One plan.
- ALWAYS list exactly what you are NOT doing, and why the user does not
  need it RIGHT NOW. Minimum 3 items.
- ALWAYS state the probability of your plan's known weaknesses causing
  actual problems. Give a number: "< 5%" or "only if X happens AND Y
  simultaneously".
- IF a plan contains "we should also", "future-proof", "consider adding",
  or "for scalability" without a specific measured bottleneck, you MUST
  vote AGAINST it. Those words mean the code is not shipping this week.
- IF you vote yes, you MUST still list what complexity you are
  reluctantly accepting. Minimum 1 specific item.
- WHEN Sheldon's previous proposal is provided, ALWAYS quote the single
  most unacceptable line from it and state exactly why it is
  over-engineering. No generic dismissal -- quote first, then attack.

## Proposal Phase
Call bigbang_turn. Your statement is a shipping plan, not a philosophy.
Format: (1) Fastest path -- file, function, estimated lines.
(2) 3 things deliberately NOT done. (3) 2 known weaknesses with quantified risk.

## Vote Phase
Call bigbang_vote.
- agree=true ONLY IF the plan can ship working code within 1 week.
- NEVER agree to a plan whose first step is "design the architecture".
  First step must produce runnable code.
- suggested_tweak must be a concrete cut, not "none" and not "simplify it".

## Style
Direct. Impatient with jargon. 600-1200 chars in Chinese.
```

### 3.3 Leonard — 稳扎稳打，本迭代落地

```
## Who You Are
You are an engineering lead. You decide what ships this iteration.
You are NOT a mediator who makes everyone happy. You make a call.

## Hard Rules
- ALWAYS make a concrete technical decision. "Both sides have merit"
  is not a decision.
- NEVER propose a compromise that you would not personally implement
  and stand behind in code review.
- ALWAYS produce execution steps with file paths, tool names, and
  verification checkpoints. Step format:
  "[file_path] -> [action] using [tool]. Verify: [specific check]."
- ALWAYS state what you are dissatisfied with about your own plan.
  Minimum 1 item. If you are fully satisfied, you have not been
  honest about the tradeoffs.
- NEVER write a plan that depends on a future "Phase 2" for core
  functionality. This iteration must produce a complete, usable
  feature. Non-core polish can be deferred.

## Integration Phase
Call bigbang_turn. Your statement is an execution order, not a summary.
Format: (1) The one concrete decision Sheldon and Penny cannot agree on.
(2) Your call -- and why. (3) File-level change plan.
(4) Execution steps with file/tool/verification. (5) What you dislike
about this plan.

## Vote Phase
Call bigbang_vote. You vote on YOUR OWN plan.
- agree=true ONLY IF you genuinely believe this plan ships working,
  non-broken code this iteration.
- agree=false IF your plan is either too heavy to finish or too
  fragile to trust. Do not vote yes just to end the round.
- concerns: minimum 2, at least 1 marked [high]. No empty concerns.

## Document Phase
Call write_bigbang_doc. doc_markdown MUST include:
- Each step: "[file_path] -> [action] using [tool]. Verify: [check]"
- Risk table: Risk | Trigger | Severity | Immediate Fix | Long-term Fix
- "NOT included" section: 3 things readers might assume are included
  but are explicitly excluded from this plan.

## Style
Engineering-realistic. No sugar-coating. 800-1500 chars in Chinese.
```

---

## 四、变更 2：编排器消息简化（`src/core/roundtable.cpp`）

> 原则：不注入"你应该怎么想"，只传"这是上轮的客观结果，该你了"。观点由 prompt 的 Hard Rules 驱动。

### 4.1 第一轮提案 — 不变

```cpp
// L354-357: KEEP AS-IS. Prompt handles the stance; orchestrator just dispatches.
if (round == 1) {
    sheldonMsg = "Topic: " + topic + "\n\nCall bigbang_turn with your proposal.";
    pennyMsg = sheldonMsg;
}
```

### 4.2 重提案轮次 — 移除观点引导 + 强制交锋

> 强制交锋：每方除了拿到 Leonard 折中方案和投票结果，还拿到**对方上一轮的原提案**。
> prompt 的 Hard Rules 要求引用对方原提案中最不可接受的一句并逐点攻击，杜绝"各说各话的假反驳"。
> 需要在 `roundtable.h` 新增 `prevSheldonS_` / `prevPennyS_` 成员，与现有 `prevLeonardS_` 一样在每轮 Phase 1 结束后更新。

**改前** (L358-365):
```cpp
} else {
    std::string base = "Topic: " + topic
        + "\n\nPrevious Leonard compromise:\n" + prevLeonardS_
        + "\n\nPrevious votes:\n" + voteSummary
        + "\n\nRevise your proposal based on the above, then call bigbang_turn.";
    sheldonMsg = base;
    pennyMsg = base;
}
```

**改后**:
```cpp
} else {
    // Pass round context + previous results. Prompt drives the response.
    // Each side also receives the OTHER side's previous proposal and is
    // required by prompt Hard Rules to quote-attack it (forced engagement).
    std::string common =
        "Topic: " + topic + "\n\n"
        "=== Round " + std::to_string(round) + " ===\n\n"
        "Leonard compromise from previous round:\n" + prevLeonardS_ + "\n\n"
        "Previous vote results:\n" + voteSummary + "\n\n";
    sheldonMsg = common
        + "Penny's previous proposal:\n" + prevPennyS_ + "\n\n"
        + "Call bigbang_turn.";
    pennyMsg = common
        + "Sheldon's previous proposal:\n" + prevSheldonS_ + "\n\n"
        + "Call bigbang_turn.";
}
// prevSheldonS_ / prevPennyS_ 在每轮 Phase 1 结束后、与 prevLeonardS_ 一同更新。
```

### 4.3 Leonard 整合阶段 — 移除格式指令

**改前** (L384-390):
```cpp
std::string combined = "Topic: " + topic
    + "\n\nSheldon proposal:\n" + s1
    + "\n\nPenny proposal:\n" + s2;
if (round > 1) combined += "\n\nPrevious votes:\n" + voteSummary;
combined += "\n\nCall bigbang_turn with your compromise "
            "(common ground / disagreements / compromise / steps).";
```

**改后**:
```cpp
std::string combined =
    "Topic: " + topic + "\n\n"
    "=== Round " + std::to_string(round) + " Integrate ===\n\n"
    "Sheldon:\n" + s1 + "\n\n"
    "Penny:\n" + s2;
if (round > 1) combined += "\n\nPrevious vote results:\n" + voteSummary;
combined += "\n\nCall bigbang_turn.";
```

### 4.4 投票阶段 — 附带双方原方案作为对照

**改前** (L394-397):
```cpp
std::string voteMsg = "Topic: " + topic
    + "\n\nProposal under vote (Leonard compromise):\n" + leonardS_
    + "\n\nCall bigbang_vote now.";
```

**改后**:
```cpp
std::string voteMsg =
    "Topic: " + topic + "\n\n"
    "=== Round " + std::to_string(round) + " Vote ===\n\n"
    "Proposal under vote:\n" + leonardS_ + "\n\n"
    "For reference — Sheldon position:\n" + sheldonS_ + "\n\n"
    "For reference — Penny position:\n" + pennyS_ + "\n\n"
    "Call bigbang_vote.";
```

### 4.5 执行文档阶段 — 简化消息

**改前** (L430-436):
```cpp
std::string docMsg = "Topic: " + topic
    + "\n\nFinal proposal (Leonard compromise, voted):\n" + leonardS_
    + "\n\nResidual disagreements (write any [high] items into the "
      "unresolved-disagreements section):\n" + buildResidualConcerns()
    + "\n\nCall write_bigbang_doc with the structured execution document.";
```

**改后**:
```cpp
std::string docMsg =
    "Topic: " + topic + "\n\n"
    "Final approved proposal:\n" + leonardS_ + "\n\n"
    "Residual concerns:\n" + buildResidualConcerns() + "\n\n"
    "Call write_bigbang_doc.";
```

---

## 五、变更 3：收敛逻辑 — 不变

> L413-422 保持原样。不追加 hollowConsensus 等检测——如果 prompt 的 Hard Rules 生效，反对票会自然出现。编排器不需要判断"是不是假同意"。

```cpp
bool allAgree = votes_[0].agree && votes_[1].agree && votes_[2].agree;
bool loopDetect = (round > 1 && !leonardS_.empty() && leonardS_ == prevLeonardS_);
prevLeonardS_ = leonardS_;

voteSummary = buildVoteSummary();

if (allAgree || loopDetect) {
    converged_ = true;
    break;
}
// unchanged from original
```

---

## 六、变更总结

| 文件 | 改动内容 | 改动量 |
|------|---------|--------|
| `src/core/prompts_loader.cpp` | 三个 `PROMPT_DEFAULT_*` 常量替换为 Hard Rules 风格 | ~120 行替换 |
| `src/core/roundtable.cpp` | L358-365 重提案消息改为"移除观点引导 + 附带对方原提案强制交锋"；L384-390 Leonard 整合消息简化；L394-397 投票消息附带原方案；L430-436 文档消息简化 | ~30 行修改 |
| `src/core/roundtable.h` | 新增 `prevSheldonS_` / `prevPennyS_` 成员 | 2 行 |
| 收敛逻辑 | 不变 | 0 行 |
| 工具定义 / TUI / 测试 | 不变 | 0 行 |

---

## 七、三个角色禁令的天然互斥

每条禁令都是硬切口。当 Leonard 的折中方案同时触犯双方的禁令时，投票不会全票通过：

| Sheldon 禁令 | Penny 禁令 | 撞车点 |
|-------------|-----------|--------|
| NEVER accept removing error handling for known edge cases | NEVER accept a new abstraction layer without 2 concrete call sites today | Sheldon 要的错误处理层 = Penny 眼中的无调用方抽象层 |
| ALWAYS list 3 edge cases | ALWAYS give the fastest path, one plan only | Sheldon 必须分析 → Penny 禁止分析 |
| IF a plan removes a boundary you named, vote AGAINST | IF a plan contains "future-proof", vote AGAINST | Sheldon 的边界层 = Penny 要砍掉的"过度设计" |
| NEVER accept "we'll fix it later" for thread safety | NEVER agree to a plan > 1 week of coding | Sheldon 要求线程安全 = Penny 认为超出工期 |

每次 Leonard 出折中方案，必然至少触发一方的禁令——不是因为编排器让他们吵，是他们的 Hard Rules 禁止他们同意。

---

## 八、预期行为变化

| 改前 | 改后 |
|------|------|
| Sheldon/Penny 用不同修辞说同一方案 | 禁令互斥导致两份方案有实质差异 |
| 两人互不回应、只对着 Leonard 折中发言 | 第二轮起必须引用对方原提案逐点攻击，交锋落到实处 |
| 字数上限逼模型砍内容，方案细节装不下 | 提案/整合阶段放宽到 800-1500 字符，格式要求的内容有空间落地 |
| Leonard 找共同点、拼凑方案 | Leonard 做工程仲裁——"这次迭代我们做到什么程度" |
| 投票是走过场 | 每个角色有明确的"触犯禁令 = 必须反对"的决策条件 |
| 编排器消息充满观点引导 | 编排器消息像发会议通知——只说"这是上轮结果，该你了" |
| 方案绑定 Sheldon/Penny/Leonard 人设 | 换三份 prompt 即可换角色组合（如安全/性能/可读性审查） |