# proJV 技能层优化待办

> 状态：第 1 条仍挂起（真机未复现出它的 400）；第 2 条里的「思考内容回传」**已修**（v0.5.9，真机复现并定位）。
> 记录时间：v0.5.7；更新：v0.5.9。

## 0. 已修：`reasoning_content` 缺失导致 400（v0.5.9）

### 现场

用户让 proJV 通过 `run_skill` 跑 bigbang，**跑完之后**出现：

```
HTTP 400: The `reasoning_content` in the thinking mode must be passed back to the API.
```

会话 DB 里的位置：`[789]` 执行文档 -> `[790]` system nudge -> `[791]` 上面这条错误。即失败的是 **v0.5.8 新增的续跑轮**。

### 实测规则（用真实 API 逐条对照得出）

| 请求内容 | 结果 |
|---|---|
| assistant 消息无 `reasoning_content` + 带 `tools` | **400** |
| 同一消息带 `"reasoning_content": ""` + 带 `tools` | 200 |
| 同一消息带任意非空 reasoning + 带 `tools` | 200 |
| 无该字段、但不带 `tools` | 200 |

即：**只要请求带 `tools`（proJV 永远带），每一条 assistant 消息都必须有这个字段**，空字符串即可。

### 根因

v0.5.8 之前技能叙述不进会话（只把 `lastDoc_` 塞进工具结果），会话里不存在"没有 reasoning 的 assistant 消息"。v0.5.8 让技能叙述以 assistant 消息落库，7 条都缺该字段，于是下一轮被拒。

同样会踩这个坑的还有：`/help` 输出、`/clear` 的 "Session cleared."、以及 400 之后我们自己写进会话的 `Error: API request failed...`（它会形成连环 400）。

### 修法

`client/models.cpp::to_json`：assistant 消息**无条件**输出 `reasoning_content`，没有思考链就发空串。

- 不给技能叙述伪造思考文本（实测非空也 200，但假 CoT 会被拼进上下文，污染模型）。
- 不改技能、不改 Agent 状态机、不动 tool 配对。
- 单测：`tests/unit/test_reasoning_content.cpp`（5 例）。

## 1. 强制工具与思考模式冲突（bigbang 每次都多跑一趟）—— 仍挂起

### 官网依据

`thinking` 默认 `enabled`；"`required` and named tool choices are not supported in thinking mode; the API returns a 400 error."

### 代码依据

`skill_config.h:54` 默认 `verb` → `skill_runner.cpp:273` 算 `force` → `sub_agent.cpp:56` 拼 `toolChoice`；全仓库从不发送 `thinking` 字段。

### 现状（v0.5.9 复核）

- 扫描全部历史会话，**只出现过一次 API 错误通知**（就是上面第 0 条那条），说明强制工具触发的 400 即使发生也是被 `requestTool()` 的兜底静默吃掉了，没有在界面上留下痕迹。
- 因此**无法从数据判断**它到底有没有发生；`04` 最初的分析依据仍是官网文字。
- 兜底判据已在过程中放宽过一次设想（`tool_choice` / `thinking` / `HTTP 400` 任一），但该改动随后回退，当前代码仍只认 `tool_choice` 字面。

### 技能统一优化时的建议

强制工具时在请求体加 `thinking: {"type": "disabled"}`（技能按手册执行，不需要思维链），并把兜底判据放宽到任意 400。命中范围只有 `bigbang`（其 `config.json` 未写 `tool_mode`），`monica` / `skill_maker` 是 auto，不受影响。

## 2. 剩余待议

| 项 | 官网说法 | 现状 | 影响 |
|---|---|---|---|
| 图片分辨率 | 单边最多 8192 px（≥15 张时 4096 px） | 只校验文件大小（30 MiB）与张数（8） | 超大尺寸图片必进 400，且会被 4xx 重试逻辑带着图重发 3 次 |

## 3. 技能优化时可以顺带看的东西

- `tool_mode` 与 `dynamic_dispatch` 只有 `verb` / `auto` 两种，配置文件没有校验；写错（例如驼峰 `toolMode`）会静默回落到 `verb`。
- `/help` 不列出已安装技能。
- v0.5.8 起 `run_skill` 与 `/<skill>` 已共用 `runSkillBlocking()`，面板/气泡/DB 一致；差别只剩"agent 触发时不伪造用户消息"。
