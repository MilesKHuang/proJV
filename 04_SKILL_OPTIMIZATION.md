# proJV 技能层优化待办

> 状态：**只记录，暂不修**。用户计划对所有技能做一次统一优化，届时一并处理。
> 记录时间：v0.5.7 + 03 文档实现之后。

## 1. 强制工具与思考模式冲突（bigbang 每次都多跑一趟）

### 现象

bigbang 能跑通，但每个动作都要发两次请求：第一次被服务端拒绝，代码兜底改成"模型自选工具"再发一次。

### 官网依据

`https://api-docs.deepseek.com/api/create-chat-completion`：

- `thinking` 对象 **默认 `enabled`**（思考模式默认开启），`reasoning_effort` 默认 `high`。
- `tool_choice`："`required` and named tool choices are not supported in thinking mode; the API returns a 400 error. Disable thinking mode first to use them."

即：**思考模式开着的时候，不允许用 `{"type":"function","function":{"name":...}}` 指定工具**。

### 代码依据

| 位置 | 内容 |
|---|---|
| `src/core/skill_config.h:54` | `toolMode` 默认 `"verb"` |
| `src/core/skill_runner.cpp:273` | `const bool force = (config_.toolMode != "auto") && !config_.dynamicDispatch;` |
| `src/core/sub_agent.cpp:56` | `req.toolChoice = force ? verb : "auto";`（force 时拼成指定函数的对象形式） |
| `src/client/deepseek.cpp` | `buildRequestBody()` 把非 `auto/none/required` 的 toolChoice 序列化成 `{"type":"function",...}` |
| `src/core/sub_agent.cpp:108-114` | 兜底：`calls.empty() && force && lastError_` 含 `"tool_choice"` -> 以 `force=false` 重发 |

全仓库没有任何地方发送 `thinking` / `reasoning_effort`（`grep` 无命中），所以服务端永远按默认的"思考开启"处理。

### 影响面（按当前 skills 目录逐个核对）

| 技能 | 配置 | 是否命中 |
|---|---|---|
| `bigbang` | `config.json` **没有 `tool_mode` 字段**，`dynamic_dispatch` 也没有 | **命中**，每个 action 两次请求 |
| `monica` | `"tool_mode": "auto"`, `"dynamic_dispatch": true` | 不命中 |
| `skill_maker`（内置） | `"tool_mode": "auto"` | 不命中 |
| 用户后续新增技能 | 若照 bigbang 的写法（不写 `tool_mode`） | 默认命中 |

量级：bigbang 一轮通常 6 个 action，8 轮就是约 48 个 action，命中时请求数翻倍。

### 隐性风险

兜底依赖 `lastError_` 里出现 `tool_choice` 这个词。官方哪天改文案，兜底失效，`doRequest` 会把那段 400 报错文本当成这一轮的产出交回 SkillRunner，技能在"看似正常"的情况下产出错误内容。

### 修复选项

| 方案 | 改动 | 代价 |
|---|---|---|
| A. 配置止血 | `skills/bigbang/config.json` 加 `"tool_mode": "auto"` | 零代码；但模型自选工具，流程确定性下降 |
| B. 请求关思考 | `force` 时在请求体加 `"thinking": {"type": "disabled"}` | 需要给 `ChatRequest` 加字段；技能用不上思考，速度更快、更省 token |
| C. 记住能力 | 首次被拒后给该 SubAgent 打标记，后续直接用 `auto` | 只省后续请求，第一次仍白跑 |

推荐 B（配合 A 作为兜底）：技能流程是"按手册走"，本来就不需要思维链。

### 怎么验证修好了

- 调试日志（Debug 构建）里不应再出现 `forced tool_choice rejected, retrying with auto`。
- 一轮 6 个 action 的请求数从 12 降到 6。

### 阻塞项

真机未验证：仓库内 `api_key.txt` 对 `api.deepseek.com` 返回 401。上述"400 会被触发"来自官网文档 + 代码走读，不是实测。

## 2. 同批发现但不在技能层的两条（另议）

| 项 | 官网说法 | 现状 | 影响 |
|---|---|---|---|
| 思考内容回传 | 请求带 `tools` 时，**所有轮**的 `reasoning_content` 都应回传并被拼进上下文 | `models.cpp::to_json` 只在 `toolCalls` 非空时回传 | 长对话里思考链断裂，模型易重复试错；全量回传会显著增加输入 token |
| 图片分辨率 | 单边最多 8192 px（≥15 张时 4096 px） | 只校验文件大小（30 MiB）与张数（8） | 超大尺寸图片必进 400，且会被 4xx 重试逻辑带着图重发 3 次 |

## 3. 技能优化时可以顺带看的东西

- `tool_mode` 与 `dynamic_dispatch` 的组合语义目前只有 `verb` / `auto` 两种，配置文件里没有校验；写错（例如 `"toolMode"` 驼峰）会静默回落到 `verb`。
- `run_skill` 路径下右侧 Agent 面板为空（SkillRunner 是 executor 局部对象），只有显式 `/<skill>` 才显示角色状态。若要让两条路径一致，需要把 runner 发布到 `skillRunner_` 并加锁。
- `/help` 不列出已安装技能。
