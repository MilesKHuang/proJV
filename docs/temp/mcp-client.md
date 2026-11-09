# TEMP 设计稿：proJV 作为 MCP Client（v3 —— 常驻 MCPManager）

> **TEMP（临时设计稿）** —— 中文、仅供评审，未定稿。
> 不进 `docs/en/` 镜像；定稿后按 `AGENTS.md` 拆成双语正式文档，本文件删除。
> 本文只做设计，不含实现。行号来自实际读取；未核实标 `[待证]`。
>
> **v3 修订**：对照 `src/` 逐条复核，订正 3 处行号漂移，补 5 处缺口（后台线程归属、
> 取消链路、三根管道、文档转立场、清单漏项），换掉 1 条恒真的无效用例。
> 明细见第 19 节。

---

## 1. 目标

proJV 作为 MCP Client，通过 **MCPManager 常驻管理** 的方式接入 MCP server：

1. **常驻进程池**：proJV 启动时拉起配置中 `enabled=true` 的 server，持有其 stdin/stdout 双向 JSON-RPC 管道；proJV 退出时按顺序优雅关闭全部 server。
2. **双协议模式**：支持 legacy（`2025-11-25`，握手式，兼容 2024-11-05 起的存量 server）与 modern（`2026-07-28`，无握手、每请求带 `_meta`）两种 MCP 协议。
3. **F4 打开 MCP 管理 UI**：每个 server 一个 Toggle（运行/停止），热启动、热停止，不重启 proJV。
4. **独立配置列表**：`projv_files/mcp.json` 作为唯一配置来源（不混入 `config.toml`，职责分离；JSON 与 MCP 生态同构，nlohmann/json 已 vendored 零新依赖）。
5. **实时上下文注入**：与 F3 的 `injectWorkspaceContext()` 同路径，把启用的 server 清单/工具名注入 system 前缀，模型可见即可调。

三条硬边界：

1. **`IProcessRunner` 一行不改**：MCP 走 MCPManager 自己的进程管理，绝不借用 `IProcessRunner` 的全局锁。
2. **任意 JSON Schema 原样透传**：不降级、不扩 `ToolParameter`。
3. **MCP 工具注册即自动可见**：走新增的 `autoToolNames_` 通道，既不依赖用户手写提示词，也不借"空 allowlist = 全放行"那条逃生口。

本版明确不做：SSE/HTTP 传输、resources/prompts 资源方法、per-tool 审批 UI、server 主动请求的应答（sampling/roots/elicitation 一律拒绝）。

---

## 2. 现状事实

| 事实 | 证据 |
|---|---|
| 执行器收的是原始 JSON 字符串 | `registry.h:9` `ToolExecutor = function<string(const string&)>` |
| 工具注册是 map 赋值，**重名静默覆盖** | `registry.cpp:5-8` `definitions[def.name] = def; executors[def.name] = executor;` |
| `ToolParameter` 只有四种 type | `models.h`：string / number / boolean / array |
| 工具 schema 由 parameters 拼出来 | `models.cpp:7-38` `toJsonSchema()`，异常回落 `{"type":"object"}` |
| **发给模型的工具表按白名单过滤** | `agent.cpp:932` `req.tools = getFilteredToolDefinitions();`，`:960-967` 空则全放行、否则按名单过滤 |
| 空白名单 = 不设限 | 同上，`if (allowedTools_.empty()) return all;` |
| 进程管理器一次一个子进程、跑完才算完 | `iprocess_runner.h`，`Run()` 持锁到子进程退出 |
| 默认超时 **30 分钟** | `iprocess_runner.h:12` `kProcessTimeoutMs = 1800000` |
| **Windows 上子进程 stderr 塞进 stdout 同一根管道** | `iprocess_runner.h:25-27` 注释；`process_runner_win.cpp:80` `si.hStdError = hOutWr;` |
| 技能子代理的白名单是真闸门 | `sub_agent.cpp:186-189` 逐个比对；`skill_runner.cpp:393-394` 按同名单注册 schema |
| 项目明文禁止加 MCP 层 | `AGENTS.md`：`Do not add an MCP layer, a plugin registry or a package manager.` |
| **F3 = 新对话，F4 目前空闲** | `main_tui.cpp:378-382` F3 绑定 `app.newChat()`；F4 无绑定 |
| **F2 打开设置，F5 打开会话，F6/F7 主题** | `main_tui.cpp:374-394` |
| **上下文注入点：`injectWorkspaceContext()`** | `app_tui.cpp:864-884`，F3 与 `/clear` 同路径 |
| **`setupTools()` 在 `initialize()` 里跑一次** | `app_tui.cpp:199-207`；注册后不重跑，配置保存只重建工具 |
| **`saveFullConfig()` 重建内置工具** | `app_tui.cpp:833-858`，其中 `:851` 调 `setupTools()`，换工作区时全部重注册 |
| **`agent->run()` 全仓库只有一个调用点** | `app_tui.cpp:528`（`runAgentTurnLoop`）；**这是 `syncTools()` 唯一需要插入的位置** |
| **`Event::F4` 在本版 FTXUI 中存在** | `external/FTXUI-7.0.3/include/ftxui/component/event.hpp:87` `static const Event F4;` |
| **底部按键提示行已排满 12 项，F4 未列入** | `main_tui.cpp:596-603`（F1-F10 + ↑↓ + Enter + Esc） |
| **技能引擎有独立 runner 与 agent dock** | `app_tui.cpp:653-716` `runSkillBlocking`；`main_tui.cpp:521` `renderAgentPanel` |

---

## 3. 关键机制：`autoToolNames_` 自动可见通道

**白话**：第一层 = 工具注册了；第二层 = 白名单放行。**注册 ≠ 可见**——`agent.cpp:932` 决定发给模型的工具表，而它按 `allowedTools_`（= 提示词那行 `tools available:`）过滤。名字没进白名单的工具，注册了也永远不被模型看见。

**现状各行机制的真实行为**（已核对代码）：

| 机制 | 注册 | 谁让它可见 | 结论 |
|---|---|---|---|
| 内置工具 | `setupTools()` | `prompts` 里手写的工具行 | 依赖提示词 |
| pytool | `python_tool_manager.cpp:174` `registerTool` | **只注册，不提交白名单** | **必须手写进提示词才可见**（`diagram_tool` 就是 `prompts_loader.cpp:54` 手写的） |
| skill 子代理 | 技能 config | 技能自己的白名单 | 与主代理无关，**且不接 MCP**（见下） |
| **MCP（新）** | `mcp.json` 声明 | **运行时才知道名字，无法预写提示词** | 必须自动可见 |

> **技能子代理不接 MCP（明确边界）**：技能工具来自技能 `config.json` 的 `tools` 数组，按名从 `ToolRegistry` 取（`skill_runner.cpp:393-394`），子代理只在 `toolWhitelist_` 内放行（`sub_agent.cpp:186-189`）。本版**约定技能白名单里不出现 `mcp__` 前缀的名字**，实现上在 `SkillRunner` 装载 `config.tools` 时跳过 `mcp__` 开头的项并打一行日志。理由：技能会以 `max_parallel`（默认 16，`skill_runner.cpp:142`）并发跑子代理，若允许它们共用同一个 MCP server，会出现多线程同时写同一根 stdin 的问题。**MCP 只服务主 Agent**——主 Agent 的工具执行天然串行（`agent.cpp:399-422` 的单线程 while 循环），无需额外加锁。

**问题的本质**：MCP 工具名是运行时从 server 的 `tools/list` 才拿到的（`mcp__fs__read_file` 这种），用户**根本没法提前写进提示词的 `tools available:`**。pytool 虽然也靠手写提示词，但至少工具名是作者定的、能预写；MCP 连这一点都做不到。

**方案：新增 `autoToolNames_` 通道，不动 `allowedTools_` 的语义。**

```cpp
bool Agent::isToolAllowed(const std::string& n) const {
    if (autoToolNames_.count(n)) return true;     // MCP / 自动可见：注册即可见
    return allowedTools_.empty() ||               // 内置：语义一字不动
           std::find(allowedTools_.begin(), allowedTools_.end(), n) != allowedTools_.end();
}
```

- `autoToolNames_` 是 `Agent` 的一个 `std::set<std::string>`，由 `MCPManager` 在注册/注销时增删（`addAutoTools` / `removeAutoTools`）
- `getFilteredToolDefinitions()`（`:960-967`）的快速返回改成 `allowedTools_.empty() && autoToolNames_.empty()`，其余走 `isToolAllowed`
- `executeTool()`（`:820-821`）的判定换成 `if (!isToolAllowed(call.name))`
- **查询时求值，不缓存**：`setSystemPrompt` / `replaceSystemPrompt` 有**三个**调用点（`app_tui.cpp:256`、`:900`、`:962`），缓存等于让"凡改提示词处都得记得重算"靠纪律维持

**为什么不能直接把名字塞进 `allowedTools_`**：`allowedTools_` 空 = 全放行。若启动时把 MCP 名塞进去，`allowedTools_` 变非空，用户"删掉整行、本意全放行"的场景下**内置工具反而全被禁**。`autoToolNames_` 独立成集合，`allowedTools_` 保持原语义，两边互不污染。

不变式：用户手改提示词、删掉 `tools available:` 整行后 → 内置工具照旧全放行（老语义），MCP 工具因在 `autoToolNames_` 里也照旧可见。**两条通道正交，互不影响。**

**pytool 后续也改走同一通道**（独立一刀，见 14 节 M2）：机制一样、体验一致；pytool 若改走它，就不再需要手写提示词。但 MCP 先做——它是唯一"名字无法预写"的机制，必须先有这条通道。

**MCP 工具调用体验 = pytool 完全一致**：模型看到一个工具名 → 调用 → 得到结果 → 回合继续。**不需要 skill 式旁路**（run_skill 那种"回合外阶段 + 合成消息"只适用于多智能体技能，普通工具天然就在回合内）。MCP 只是把 pytool 的一次性 spawn 换成常驻进程。

---

## 4. 协议范围：双模式 Client

> **依据**：官方规范原文取自 GitHub 规范仓库 `modelcontextprotocol/modelcontextprotocol`
> `docs/specification/` 下的 `2025-11-25/`（legacy 基线）与 `2026-07-28/`（最新）。
> 关键文件：`2025-06-18/basic/lifecycle.mdx`、`2025-06-18/basic/transports.mdx`、
> `2025-06-18/server/tools.mdx`、`2026-07-28/basic/versioning.mdx`、
> `2026-07-28/basic/transports/stdio.mdx`、`2026-07-28/server/tools.mdx`、`2026-07-28/changelog.mdx`。
> 线上页面：`modelcontextprotocol.io/specification/<版本>/...`。实现前请以这些原文为准，不要凭记忆。

### 4.1 双模式

| 模式 | 协议版本 | 握手 | 用途 |
|---|---|---|---|
| A（默认，legacy） | **`2025-11-25`**（发给 server 的 `protocolVersion` 用它；兼容 2024-11-05 / 2025-03-26 / 2025-06-18 的 server） | `initialize` + `notifications/initialized` | 生态存量 server |
| B（modern） | **`2026-07-28`** | 无握手；**每个请求带 `_meta`** | 新版 server |

**配置里 `protocol = "stateful" | "stateless"` 二选一，默认 `"stateful"`。** 不做自动探测。

> **偏离官方之处（有意为之）**：2026 stdio 规范推荐先发 `server/discover` 探测时代，失败再回退 `initialize`。本版用配置显式声明代替自动探测——连一个 server 只应尝试一次，猜错的代价比"用户写一行配置"高。文档写死"配置声明协议版本"。

### 4.2 双向性：两个时代的差别（重要）

**模式 A（legacy，2025-11-25 及更早）**：MCP 是双向协议，server 可主动发请求
`sampling/createMessage`、`roots/list`、`elicitation`。

- 做法：`initialize` 里声明 `capabilities: {}` —— "我不支持任何额外能力"。规矩的 server 不会来借模型/问目录。proJV 没有理由声明 `sampling`（等于把模型借给外部进程用）。
- 不守规矩的 server 发来请求：MCPManager 记进错误信息，不假装能回。

**模式 B（modern，2026-07-28）**：**server 不再主动发请求**。规范（`2026-07-28/basic/transports/stdio.mdx`）明文：

> The server **MUST NOT** write JSON-RPC _requests_ to `stdout`. Server-to-client interactions are carried in `InputRequiredResult` replies.

服务端需要额外输入时，返回 `resultType: "input_required"` 的结果，由 client 再发一次请求（MRTR 模式）。**所以模式 B 下不会有 `sampling` / `roots` / `elicitation` 请求进来**；取而代之的是：`tools/call` 可能返回 `resultType: "input_required"`。同时 `2026-07-28/changelog.mdx` 明确把 **Roots / Sampling / Logging 标记为 deprecated**，并**删除了 `ping`、`logging/setLevel`、`notifications/roots/list_changed`**。

**本版对 `input_required` 的处理**：识别到即返回一条明确的工具错误给模型（`MCP server 'x' requires additional input (MRTR) — not supported in this version`），不当作成功、不静默吞掉。

### 4.3 必须支持的方法

| 方法 | 模式 A | 模式 B |
|---|---|---|
| `initialize` + `notifications/initialized` | ✅ | ❌ |
| `tools/list` | ✅ | ✅ |
| `tools/call` | ✅ | ✅ |
| `ping` | ✅（可选发送） | ❌ 已从规范删除 |
| `notifications/cancelled` | ✅ | ✅ |
| `notifications/progress` | 读掉丢弃 | 读掉丢弃 |
| `notifications/tools/list_changed` | 读掉丢弃 | 读掉丢弃 |
| `server/discover` | ❌ | 可选（本版不发，用配置声明代替） |

### 4.4 模式 B 的报文形态（2026-07-28，必须逐字对齐）

**每个请求的 `params._meta` 必须含**：

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "tools/list",
  "params": {
    "_meta": {
      "io.modelcontextprotocol/protocolVersion": "2026-07-28",
      "io.modelcontextprotocol/clientCapabilities": {},
      "io.modelcontextprotocol/clientInfo": { "name": "proJV", "version": "<PROJV_VERSION>" }
    }
  }
}
```

- `protocolVersion` 与 `clientCapabilities` **必填**；缺失 → server 回 `-32602`（Invalid params）。
- 版本不被接受 → server 回 **`UnsupportedProtocolVersionError`**。client 收到即标记该 server 配置错误并停止（不反复重试）。

**所有 result 必须带 `resultType`**：

- `"complete"` = 正常结果
- `"input_required"` = 需要 client 追加输入（MRTR）

老 server 若省略 `resultType`，client **必须**按 `"complete"` 处理（向后兼容）。

**`tools/list` 结果新增必带字段**（CacheableResult 接口）：`ttlMs`（毫秒，可为 null）、`cacheScope`（字符串，可为 null）。client 只读取、不依赖；缺失时不报错。

**`tools/call` 结果的 `structuredContent` 可以是任意 JSON 值**（不只是 object），不得假定为 object。

### 4.5 分帧

**换行分隔 JSON**（不是 LSP 的 `Content-Length`）。规范原文：每条消息是单行 JSON，`\n` 结尾，消息内不得有裸换行。`nlohmann::json::dump()` 不产裸换行，出站天然安全；入站按 `\n` 切，单行上限 1 MiB，超限判违约并断开。

**stderr 只用于日志**，client 读掉丢弃、不得当作协议数据（这也解释了 Windows 把 stderr 并进 stdout 时的污染风险，见 C3）。

### 4.6 明确排除

- **SSE / Streamable HTTP**：要流式解析 + 断线重连，失败模式与 stdio 完全不同
- **`tools/list` 分页**：`nextCursor` 非空则**整台拒绝注册**。静默只注册第一页会让模型对缺失的工具名反复猜，比报错贵得多。**这是本版的明写缺口**，见第 12 节。
- **resources/prompts、MRTR 的完整交互**：只识别 `input_required` 并报错，不实现多轮 MRTR。

---

## 5. Schema：原样透传

`ToolDefinition` 加一个字段：

```cpp
std::string inputSchemaJson;   // 非空则 toJsonSchema() 原样发出
```

`toJsonSchema()`（`models.cpp:7`）首行短路即可，约两行。

**为什么不降级**：有人会想把 MCP 的嵌套 object 降成 `type: "string"` 让模型自己拼 JSON。不成立——`ToolExecutor` 收的本来就是原始 JSON 字符串，`ToolParameter` 只决定模型看到的形状，**从不参与传输**。透传比写降级逻辑还省事。

降级的实际后果：模型把 `{"a":{"b":1}}` 写成 `"{\"a\":{\"b\":1}}"`（带双重转义）塞进 string 字段，server 的 schema 要 object，回 `invalid params`，而错误文本在 server 侧。

**注册期校验**：`parse` 一次 `inputSchema`，不是 object 或解析失败 → **只拒这一个工具**并记一行。否则 `to_json` 每次构建请求都会回落成 `{"type":"object"}`，properties 与 required 全丢，模型自己编参数名。

---

## 6. 生命周期：MCPManager 常驻进程池

### 6.1 为什么常驻

`IProcessRunner` 的全局锁（一次一个子进程、跑完才算完）不适合 MCP：

1. **有状态 server 用不了**：playwright 这类把会话状态挂在进程里，每次新连接等于会话全丢
2. **每次 `tools/call` 都重建连接**：初始化握手 + 身份协商每次都重复，慢且无谓
3. **`Run()` 持锁到子进程结束**：一个挂住的 server 占着 runner 全局锁，`exec_shell` 与所有 skill worker 排队

**做法**：MCPManager 自己管理子进程生命周期，**绝不借用 `IProcessRunner`**（`iprocess_runner.h:40-52` 明写全进程一个 runner、持锁、Cancel 指"当前进程"）。MCP 不碰它，等于"不许把 runner 改成池"那条规则**不适用**——那条保护的是 shell / pytool 的一次性串行模型。

### 6.2 MCPManager 的结构（无锁串行，贴合项目线程哲学）

```
src/core/mcp_manager.h/.cpp
├── McpServerHandle                  一个 server 的全部状态
│   ├── pid / jobObject（Win）| pgid（Linux）
│   ├── stdinPipe / stdoutPipe
│   ├── protocol（stateful | stateless）
│   ├── connState（Stopped | Connecting | Handshaking | Ready | Dead）
│   │     └── std::atomic<ConnState>
│   ├── cancelRequested（std::atomic<bool>）
│   └── tools（已注册的 ToolDefinition 快照）
├── McpManager
│   ├── loadFromJson()               读取 projv_files/mcp.json
│   ├── start(name) / stop(name) / restart(name)
│   ├── shutdownAll()                proJV 退出时关闭全部
│   ├── callTool(serverName, toolName, args)   tools/call 的唯一入口
│   └── syncTools   (ToolRegistry&, Agent&)    回合边界：注册/注销+autoToolNames_
```

#### 线程归属（每个动作跑在哪个线程，必须写死）

| 动作 | 执行线程 | 说明 |
|---|---|---|
| `loadFromJson()` | 主线程（`TuiApp::initialize`） | 只读文件、建配置结构，零进程操作 |
| `start` / `stop` / `restart`（spawn/kill 进程） | **后台工作线程** | spawn/握手/`tools/list` 可能耗时（>1s），**绝不在 TUI 绘制线程做** |
| `syncTools()`（写 ToolRegistry + `autoToolNames_`） | **Agent 线程** | 见下"工具表写入只在回合边界" |
| `callTool()` | **Agent 线程**（`executeTool()` 本来就在此线程） | 阻塞读 stdout 到响应/超时/EOF |
| `shutdownAll()` | 主线程（`TuiApp::shutdown`）或退出的 backstop | 逐一 EOF→等待→强杀 |
| UI 读状态 | TUI 线程 | 只读 `connState`（原子），不改共享数据 |

**关键规则：TUI 线程永不做阻塞 I/O。** F4 的 Toggle 只投递请求（改原子标志 / 入队），真正的 spawn/kill 在后台线程完成；面板渲染只读原子状态，不 join、不等待。

#### 后台线程归谁管：每个 server 一个 `worker_`（v3 新增，硬约束）

**问题**：上表只说"`start` / `stop` / `restart` 跑在后台线程"，没说这条线程**谁持有、谁 join**。`start()` 的语义是"立即返回"，实现上就必须起一条线程；不起就阻塞 TUI 线程，起了不 join 就有两处释放后使用（use-after-free）：

1. **退出时**：`TuiApp::shutdown()`（`app_tui.cpp:757-773`）会 `delete agent`，`~TuiApp` 随后销毁 `std::unique_ptr<McpManager>`。此刻某条 worker 若还在 `tools/list` 的读循环里，它要写回的 `McpServerHandle` 已经没了。
2. **快速"关→开"**：`restart()` = stop + start。若 stop 没等上一条 worker 收工，两条线程会同时持有同一个 handle 的管道。

**做法**：`McpServerHandle` 持一个 `std::thread worker_`，四条规则写死：

| 入口 | 对 `worker_` 的动作 |
|---|---|
| `start(name)` | 先 `joinWorker()`（若 `joinable`），再 `worker_ = std::thread(...)` |
| `stop(name)` | 置 `cancelRequested` → `joinWorker()` → 关管道 → 等 2s → 强杀进程树 |
| `restart(name)` | `stop()` 内部已 join，直接接 `start()` |
| `shutdownAll()` / `~McpManager` | 逐个 `stop()`，即逐个 join |

`joinWorker()` 就是 `if (worker_.joinable()) worker_.join();`。**不引入 detached 线程、不引入工作队列、不引入 mutex。**

这条是 `McpManager` 无锁承诺的**前提**：worker 的生命周期被 `start` / `stop` / `shutdownAll` 三个同步点框死，不存在"两条线程同时摸同一个 handle"的窗口，所以 handle 内部才不需要互斥锁。

> **明确禁止 `std::thread(...).detach()`**：detach 之后没有任何人能在销毁前等它，第 1 条事故必然发生，而且只在退出时偶发——正是最难查的一类。

#### 工具表写入只在回合边界（数据竞争的唯一防线）

**为什么必须这样**：`ToolRegistry` 的 `definitions` / `executors` 是裸 `unordered_map`，无锁（`registry.cpp:6-7`）；`Agent::autoToolNames_` 是裸 `std::set`。若后台线程在 `registerToolIfAbsent` / `unregisterByNamePrefix` 时，Agent 线程正在 `getFilteredToolDefinitions()` 里遍历同一张表（`agent.cpp:960-967`），就是**一边遍历一边 insert/erase（可能 rehash）= 未定义行为**。

**所以拆成两段**：

1. **后台线程只碰进程**：`start/stop/restart` 只负责 spawn/kill、握手、`tools/list`，结果写进 `McpServerHandle::tools`（该 server 自己的快照），**绝不碰 ToolRegistry**。
2. **Agent 线程在回合边界同步**：`TuiApp::runAgentTurnLoop` 每轮 `agent->run()` 之前调用 `mcpManager_->syncTools(tools, *agent)`，在 Agent 线程里一次性完成注册/注销 + `autoToolNames_` 增删。

好处：工具表的唯一写入方就是 Agent 线程，**无需锁**；F4 关掉 server 后，模型在"下一个回合"立刻看不到该工具（本来每回合就要重建请求，零感知延迟）。

> 这是本设计"无锁"承诺的前提。没有这条，第 6.2 节的"不引入 mutex"不成立。

#### F4 Toggle 只在空闲时可用 + 写回 mcp.json

- **`!app.isBusy()` 才允许 Toggle**（`app_tui.h:100` 现成的 `isBusy()` = `agentThreadRunning_`）。busy 时面板显示 `busy — finish the current turn first`，不接受操作。这样后台 spawn/握手不会与正在跑的回合交错。
- **Toggle 写回 `mcp.json` 的 `enabled` 字段**，与项目现有 F 键惯例一致（F2 设置 `saveApiKey`/`saveConfig`、F6 主题 `saveConfig`、F10 模型 `saveConfig`，见 `app_tui.cpp:826/846/982/301`）。即：F4 Toggle 不是"临时开关"，是持久配置修改。
- 写入时机在 idle，故与 Agent 线程无并发。

> **已知遗留（不在本刀范围）**：F2 设置保存走 `saveFullConfig → setupTools() → registerTool`（`app_tui.cpp:851`），当前**没有 idle 检查**，在回合运行中保存设置同样存在工具表并发风险。这是既有问题，MCP 不引入、也不在本刀修复；建议单独记一条 issue。

#### 为什么不需要给管道加锁

`callTool()` **只有一个调用方：主 Agent 线程**。主 Agent 的工具执行是单线程 `while` 循环（`agent.cpp:399-422`），一次只有一个工具在跑；而技能子代理**不接 MCP**（见第 3 节明确边界）。因此同一个 server 的 stdin/stdout 在同一时刻只被一个调用占用，**天然串行，不需要互斥锁、也不需要排队状态机**——这与项目"尽可能无锁"的哲学一致。

MCPManager 里唯一的跨线程共享是**生命周期状态**（`connState`、`cancelRequested`），它们用 `std::atomic` 即可，和项目现有的 `Agent::cancel()` / `std::atomic` 写法相同。**不引入 mutex、不引入工作队列。**

> 上一版担心"技能并发打同一 server"——通过第 3 节的"技能不接 MCP"边界从根上消除了，所以锁的方案作废，改用原子位。

#### 取消与生命周期（分清两件事）

| 事件 | 取消在途调用？ | 影响 server 进程存活？ |
|---|---|---|
| **ESC 取消回合** | ✅ 发 `notifications/cancelled`（带原请求 id），`callTool` 轮询 `cancelRequested` 即时返回 | ❌ **不杀 server**，进程照常常驻 |
| **F4 Toggle 关** | ✅ 终止在途调用 | ✅ 关管道→等 2s→强杀进程树 |
| **proJV 退出** | ✅ | ✅ `shutdownAll()` 逐个收尾 |
| F4 Toggle 开 / proJV 启动 | — | ✅ spawn |

**ESC 只作用于"这一次调用"，不碰 MCP 生命周期**：它把当前 `tools/call` 的 `cancelRequested` 置位并通知 server 放弃该请求，server 本身继续 Ready，下一回合还能用。只有 **F4 关停** 和 **proJV 退出** 才真正结束 server 进程。

1. **ESC 取消回合**：`TuiApp::cancelTurn()`（`app_tui.cpp:745`）→ `Agent::cancel()`（`agent.cpp:549`）→ `ToolRegistry::cancelAll()`（`registry.cpp:47`）。`cancelAll()` 只做两件事：置 `cancelRequested_`、调 `procRunner_->Cancel()`。

   **v3 订正：不存在、也不需要"cancel hook"插槽。** v2 写的"MCPManager 注册的 cancel hook"在代码里没有任何挂载点（`ToolRegistry` 只有 `registerTool` / `execute` / `cancelAll`，无扩展接口）。链路靠**轮询一个已经公开的标志**打通，`registry` 一行不改：

   - `ToolRegistry::isCancelled()` 本来就是 `public const`（`registry.h:23`）；
   - MCP 的 executor lambda 在 `TuiApp` 里创建（与 `app_tui.cpp:73` / `:151` 的现有工具同一写法），直接捕获 `this` 把轮询回调传下去：

     ```cpp
     tools.registerToolIfAbsent(def, [this, server, tool](const std::string& args) {
         return mcpManager_->callTool(server, tool, args,
                                      [this] { return tools.isCancelled(); });
     });
     ```

   - `callTool()` 阻塞读 stdout 时以 **100ms** 为周期调一次 `isCancelled()`；命中即向该 server 写一条 `notifications/cancelled`（带在途请求 id）并返回取消结果。
   - 时序天然正确：`cancelAll()` 先置标志再碰 runner（`registry.cpp:47`）；`Agent::newTurn()` 用 `toolRegistry.resetCancel()`（`agent.cpp:560`）在下一回合清位。
   - **不要**依赖 `procRunner_->Cancel()` 来中断 MCP：它只作用于 runner 的"当前子进程"，而 MCP 的子进程不归 runner 管。**server 进程不动**，`connState` 仍是 `Ready`。
2. **F4 关停 server**：`stop(name)` 置 `cancelRequested`，终止在途 `callTool`（返回明确错误给模型），再关管道→等 2s→强杀进程树。
3. **proJV 退出**：`shutdownAll()` 同上逐个收尾；`main_tui.cpp:722-725` 的 backstop 再兜一次。

> `cancelRequested` 用 `std::atomic<bool>`，`connState` 用 `std::atomic<ConnState>`。**整个 MCPManager 不持有跨线程互斥锁**——跨线程只通过原子标志通信。

#### 进程控制与三根管道（v3 补充）

Windows：CreateProcess + Job Object（`TerminateJobObject` 杀整棵树）。
Linux：fork/exec + setpgid（`killpg` 杀整组）。

与 `IProcessRunner` 的实现平行，**但不复用**——runner 是"一次一个子进程 + 全局锁"，MCP 需要常驻多进程。

**必须建三根管道（stdin / stdout / stderr），不要把 stderr 并进 stdout。**
`process_runner_win.cpp:80` 写 `si.hStdError = hOutWr`，因为它只有一条读线程、输出全是给人看的文本。MCP 没有这个约束：stdout 是纯协议流，server 往 stderr 打一行日志就会插进报文中间。三根管子之后：

- **stdout** 只喂 `mcp_protocol` 的分帧器；
- **stderr** 单独起一条线程 drain，逐行交给 `debugLogf`（前缀 `[mcp:<server>]`），**永不参与解析**；
- 于是 C3（跳过解析不了的行）仍然保留，但它从"每天都得忍的缺陷"降级成"只在 `badline` 用例里触发的防御"。

**两处照抄 `process_runner_win.cpp`，一处必须反着做**：

| 事项 | 现有 runner | MCP 怎么做 | 理由 |
|---|---|---|---|
| 黑窗口 | `:94` `CREATE_NO_WINDOW` | **照抄** | 否则每起一个 server 闪一个控制台窗口 |
| 孤儿进程 | `:46-50` Job Object `KILL_ON_JOB_CLOSE` | **照抄** | `fork` 用例要验的就是这个 |
| 命令行 | `:86` 包一层 `cmd.exe /c`（为了 `cd` / `set`） | **不包 shell**，直接 argv exec | 多一层 shell 会吃掉或插入换行，打乱分帧与 `notifications/cancelled` 时序；`command` + `args` 由 MCPManager 自己按平台规矩加引号 |

#### 关闭序列（纠正：MCP 没有 `shutdown` 方法）

**规范中不存在 `shutdown` 请求方法。** 正确序列（`2025-06-18/basic/lifecycle.mdx` 与 `2026-07-28/basic/transports/stdio.mdx` 一致）：

1. 关闭 server 的 `stdin`（发 EOF）——这是"我这边不再发消息了"的标准信号；
2. 等 server 自行退出，最多 2s；
3. 未退出 → `SIGTERM`（Linux）/ 关闭 job 或 TerminateProcess（Windows）；
4. 仍不退 → `SIGKILL`（Linux）/ `TerminateJobObject`（Windows）兜底。

**模式 B 额外要求**：2026 stdio 规范要求 server **MUST NOT** 向 stdout 写请求；client 侧无需发送任何"关闭请求"，关 stdin 即可。

### 6.3 四条硬条件

| # | 条件 | 不做的后果 |
|---|---|---|
| C1 | 每个 server 用自己的超时（默认 120s），绝不用 `kProcessTimeoutMs`（30 分钟） | 一个挂住的 server 占着 MCPManager 的管道，模型等 30 分钟 |
| C2 | server 收到 EOF 不保证退出；超时 `Cancel()` 后若 stdout 里已有 id 匹配且可解析的响应 → **按成功计**并记一行 warn | 有响应但慢的 server 被当成失败，工具结果丢失 |
| C3 | **跳过解析不了的行与不认识的 id，绝不中止** | 坏 server 往 stdout 打非协议文本。注意 v3 起 stderr 有独立管道（见 6.2「三根管道」），本行只防不守规矩的 server，不再是 Windows 常态 |
| C4 | `tools/list` 的 `nextCursor` 非空 → **整台拒绝注册** | 静默只注册半套工具，模型对缺失的工具名反复猜 |

另两条：

- 首次调用失败即标记该 server **dead**，后续返回一行稳定错误、不再 spawn（否则连续两次业务失败会触发 `ERROR_CONSECUTIVE_FAILURES`，关掉整轮工具能力）
- 超时/异常时**点名** `stdout` 中带 `method` 且无法应答的未处理消息。例如模式 A 下 server 发 `sampling/createMessage` → `MCP server 'x' sent a request we cannot answer: sampling/createMessage`；模式 B 下 server 违规发请求 → `MCP server 'x' violated protocol: wrote a request on stdout`。只写 "did not answer" 会让运维永远猜不到是协议能力缺口

### 6.4 WorkDir

注册时绑定**启动时的工作区**，打一行 `[mcp] server 'fs' workDir=<path>`。配置里**不提供 `cwd` 键**（少一个旋钮、少一条优先级规则），文档写死"换工作区必须重启"。

理由：`app_tui.cpp:853` 的 `saveFullConfig` 会重建内置工具以跟随新工作区，MCP 进程不会。不写清楚就会出现"以为在 B 项目里写文件，实际写进 A 项目"。

---

## 7. 安全：fail closed

- `trusted` 默认 **false** → **不 spawn、不注册**，启动日志一行 `[mcp] skip 'x' (trusted=false)`
- `trusted = true` 是唯一安全旋钮。当前只有一个对话角色（`prompts_loader.cpp` 只 seed `assistant.md` + 内部 `compactor.md`），所以**不设 `roles` 键**——YAGNI，未来真有多角色再加可选键（默认全角色）
- **不做 per-tool 审批 UI**：那要动 `approval_logic.*` + TUI，本版收益不抵
- `validateToolPath` 只约束内置文件工具；**MCP 工具的副作用面完全由 `trusted` 一个配置键把关**（`trusted=false` 根本不 spawn），没有第二道运行时闸。这点必须写进用户文档

---

## 8. 配置：独立 `projv_files/mcp.json`

**与 prompts 同一套做法：仓库默认不放真实配置，程序启动时检查并按需创建。**

- 仓库里**不提交** `mcp.json`（`.gitignore` 加一行 `mcp.json`）。
- `TuiApp::initialize()` 里调用 `McpManager::ensureConfigFile()`：文件不存在则写入一个空模板 `{"servers": []}` 并打日志（镜像 `ensureDefaultPrompts()` 的行为，`app_tui.cpp:249`）。
- 用户自行编辑这个文件添加 server，或用 F4 面板查看/开关。

**模板内容（程序创建 / 文档示例）**：

```json
{
  "servers": [
    {
      "name": "fs",
      "command": "npx",
      "args": ["-y", "@modelcontextprotocol/server-filesystem", "."],
      "env": { "TOKEN": "..." },
      "protocol": "stateful",
      "trusted": true,
      "enabled": true,
      "timeout_ms": 120000
    }
  ]
}
```

**为什么独立文件而不是 `config.toml` 的 `[[mcp.servers]]`**：

- **职责分离**：`config.toml` 是 proJV 自身的运行参数（模型、主题、路径）；`mcp.json` 是"要连接的外部进程清单"，生命周期不同（会热增删、会被 F4 改），schema 也不同
- **安全边界**：MCP 配置可能含 server 的 TOKEN。**实现时须在 `.gitignore` 加一行 `mcp.json`**（现在只忽略了 `config.toml`，见 `.gitignore:33`）。默认不提交、程序按需创建，避免把含 TOKEN 的文件推进仓库
- **生态同构**：MCP 本身就是 JSON-RPC / JSON Schema，配置文件用 JSON 零学习成本；nlohmann/json 已 vendored，不引入新解析器

**文件位置（v3 补写，用户最容易误解的一点）**：`getProjvDir()` 返回的是 **exe 目录**下的 `projv_files/`（`config.cpp:36-38`），**不是工作区**。工作区是 `getWorkspaceDir()`（`config.cpp:26-34`，= 启动目录）。所以 `projv_files/mcp.json` 与 `config.toml`、`prompts/`、`skills/`、`sessions/` 同级，是**安装级全局配置**。

由此得到一条必须并排写进用户文档的结论：

| 维度 | 作用域 |
|---|---|
| `enabled` 开关（F4 Toggle 写回的就是它） | **全局**：在 A 项目里关掉 `fs`，切到 B 项目仍然是关的 |
| server 进程的 cwd | **本次启动的工作区**（见 6.4） |

缺了这张表，用户一定会问"我明明开着怎么没生效"。

**字段校验**：

- `name` 必填且唯一；重复名启动日志打一行并跳过后者
- `command` 必填；空则跳过该 server
- `protocol` 只接受 `"stateful"` / `"stateless"`（默认 `"stateful"`），非法值启动日志打一行并整台跳过
- `trusted` 默认 false；`enabled` 默认 false（必须显式打开）
- `timeout_ms` 默认 120000

**热加载**：`enabled` / `trusted` 的改动**立即生效**（F4 UI Toggle 就是热启动/热停止）；`command` / `args` / `env` / `protocol` / `timeout_ms` 的改动**重启才生效**（进程已 spawn，改参数等于重建连接，不值得）。

---

## 9. F4 MCP 管理 UI

### 9.1 触发与布局

- **F4**（`main_tui.cpp:378-382` 处 F4 目前空闲）→ 打开 MCP 管理面板（模态 overlay，与 F2 设置面板同款交互）
- 布局：

```
┌─ MCP Servers ──────────────────────────────┐
│ [✓] fs        filesystem   stateful  ready │  ← Toggle + 状态
│ [ ] playwright browser     stateful  dead  │
│ [✓] gateway   (stateless)  stateless ready │  ← 协议模式标注
│                                           │
│  <Toggle> <Restart> <Done>                 │
└────────────────────────────────────────────┘
```

### 9.2 交互语义

- **只在空闲时可用**：面板打开时若 `app.isBusy()` 为真，Toggle/Restart 按钮置灰并提示 `busy — finish the current turn first`（`isBusy()` = `agentThreadRunning_`，`app_tui.h:100`）。理由见 6.2"工具表写入只在回合边界"。
- **Toggle 写回 `mcp.json`**：改 `enabled` 字段并落盘（与 F2/F6/F10 写 config 的惯例一致）。开 = `MCPManager::start(name)` 热启动；关 = `MCPManager::stop(name)` 热停止。
- **Restart**：`MCPManager::restart(name)`（stop + start），用于 server 卡死/配置变更后重连。
- **状态列**：`Stopped / Connecting / Handshaking / Ready / Dead`，来自 `McpServerHandle::connState`。
- **不提供 `/mcp` 斜杠命令**：F4 面板是唯一的开关入口，避免与 `skill_trigger` 的保留字解析（`skill_trigger.cpp:27`）纠缠。

### 9.3 与工具注册的关系

- **Toggle 开**：后台线程 spawn + 握手 + `tools/list`，结果存进 `McpServerHandle::tools`；**下一个回合边界**由 Agent 线程 `syncTools()` 注册进 ToolRegistry 并加入 `autoToolNames_`。
- **Toggle 关**：后台线程杀进程；**下一个回合边界** `syncTools()` 注销该 server 的全部工具（`ToolRegistry::unregisterByNamePrefix("mcp__fs__")`）+ 移出 `autoToolNames_`。
- **生效时机**：下一个模型请求看不到已关停的工具。**不打断当前回合**——正在执行的 `tools/call` 会等到超时或返回。
- **重要**：注册/注销**绝不在后台线程执行**（会与 Agent 线程遍历冲突），一律在 Agent 线程的 `syncTools()` 里做，见 6.2。

---

## 10. 实时上下文注入

### 10.1 注入点

与 F3 / `/clear` 同路径：**`injectWorkspaceContext()`**（`app_tui.cpp:864-884`）后面追加一段：

```cpp
// MCP context: which servers are up and which tools exist.
void TuiApp::injectMcpContext() {
    if (!mcpManager_) return;
    std::string ctx = mcpManager_->buildContextSnippet();
    if (!ctx.empty()) {
        agent->addPersistedMessage(Message::System(ctx));
        debugLogf("[TuiApp] Injected MCP context (%zu chars)", ctx.size());
    }
}
```

### 10.2 注入内容

```
[MCP available tools]
- mcp__fs__read_file: read a file inside the workspace
- mcp__fs__write_file: write a file inside the workspace
```

格式：每行 `- <注册名>: <描述>`。**只列 `Ready` 状态的 server**（Connecting / Dead 不列，模型看不到将调不通的工具）。

> 注：MCP 工具已通过 `autoToolNames_` 进入模型工具表（见第 3 节），此注入是**第二份可见性**——把工具清单以文本形式放进上下文，帮助模型理解这些工具属于哪个外部服务、何时该用。两处一致：注册表进 `tools` 字段，注入进 system 文本。

**触发时机**：
- proJV 启动（`initialize()` 末尾）
- F4 Toggle 开/关之后（此时必为 idle，见 9.2）
- F3 新对话 / `/clear`（`reinitConversation()` 里已经调 `injectWorkspaceContext()`，加一行 `injectMcpContext()`）

### 10.3 为什么既要工具表又要注入

- 工具表（`autoToolNames_` → `getFilteredToolDefinitions()`）保证模型**能调**——这是硬通道。**这是主要机制**：工具是否可见、能不能调，完全由它决定。
- 注入只是**一句说明**（这些 MCP 工具属于哪个 server、用途是什么），帮模型判断**何时用**；即使注入缺失，模型仍能从工具表看到并调用。
- 两者独立：注入是"说明"，不是"权限"。

### 10.4 注入不改变消息结构（澄清）

注入就是一条普通的 **system 消息**，和现有 `injectWorkspaceContext()` 注入的"目录树 + AGENTS.md"完全同一性质（`app_tui.cpp:871/880` 走的是 `addPersistedMessage(Message::System(...))`）：

- **只在对话起点落一次**（启动 / F3 / `/clear` / F4 Toggle 后），不是每个回合都追加；
- **每回合请求的消息数量不变**——它只是上下文最前面多一个 system 块，`buildChatRequest()`（`agent.cpp:924-958`）把它和固定 system 块放在一起，不动后面的 user/assistant/tool 消息；
- 内容极小（每工具一行），对 token 预算无实质影响。

换句话说：**不注入的唯一后果是模型不知道这工具属于哪个服务、何时该用**，而不是"调不了"。调不调得动只看工具表。

---

## 11. 测试

不联网、不依赖 python。**新增一个独立 CMake target `fake_mcp_server`**（它是被 spawn 的独立进程，不能是 `projv_tests` 内的一段代码），单测通过绝对路径 spawn 它。

### 11.1 假 server（模式）

实现为一个可执行的小程序（**独立的 CMake target `fake_mcp_server`**，C++ 单文件，不得依赖 python），通过 `--mode=<name>` 选择行为。测试通过编译期宏（如 `FAKE_MCP_SERVER_PATH`，由 CMake `target_compile_definitions` 注入 `$<TARGET_FILE:fake_mcp_server>`）拿到它的绝对路径：

- `stateful`：完整 `initialize` 响应 + `notifications/initialized` + `tools/list` / `tools/call`（模式 A）
- `stateless`：无握手，校验每个请求 `_meta` 三件套，回 `resultType: "complete"`（模式 B）
- `badmeta`：模式 B 下模拟 server 不接受版本，回 `UnsupportedProtocolVersionError`
- `inputreq`：模式 B 下 `tools/call` 回 `resultType: "input_required"`
- `ok`：正常应答
- `dup`：配置里出现两个同名 server（验证第 8 节"重复名跳过后者"）
- `badline`：响应中混入非 JSON 行（模拟 stderr 污染）
- `noexit`：收到 EOF 后不退出（验证 2s 后强杀）
- `hang`：收到请求后不回（验证超时）
- `slowcall`：`tools/call` 延迟 3s 才回（验证 C2 慢响应按成功计）
- `softfail`：`tools/call` 回 `isError:true` 的 result（业务失败，非协议失败）
- `fork`：spawn 后派生子进程再让父退出（验证孤儿清理）

### 11.2 必须覆盖

1. 注册：`tools/list` → `mcp__fake__echo` 出现在工具表
2. 自动可见：`trusted=true` 的 server 注册后，`isToolAllowed("mcp__fake__echo")` 为 true（**不依赖提示词**，成对断言：提示词里没有它，仍可见）
3. 逃生口：提示词删掉 `tools available:` 整行后，`read_file` 仍可用，MCP 工具也仍可用（两条通道正交）
4. 坏行：`badline` 模式下正常行照常解析，进程不中止
5. 不退出：`noexit` 模式下按 C2 判成功、不卡满超时
6. 挂死：`hang` 模式下 1s 超时返回错误，进程树被杀
7. 软错误：`softfail` 连调两次，两次结果 `isError` 均为 false、首 6 字符不是 `Error:`
8. 重名（v3 重写）：**v2 的写法恒真、测不到东西**——注册名恒为 `mcp__<server>__<tool>`，与内置 `read_file` 结构上不可能相同。改为验第 8 节的配置校验：`mcp.json` 里写两个 `name` 都是 `fs` 的条目 → 启动日志打一行、**只 spawn 一个进程**、工具表里只有一份 `mcp__fs__*`、`connState("fs")` 为 `Ready`。`registerToolIfAbsent` 的撞名保护**保留为代码防御**（M2 的 pytool 通道会复用它），但不作为本版的断言
9. **常驻**：`stateful` 模式握手成功后，`tools/call` 复用同一连接（断言只 spawn 一次）
10. **无状态**：`stateless` 模式直接 `tools/list`，不握手；请求 `_meta` 三件套存在
11. **版本拒绝**：`badmeta` 模式回 `UnsupportedProtocolVersionError`，client 标记 dead、不重试
12. **MRTR**：`inputreq` 模式返回 `resultType: "input_required"`，client 返回明确错误而非成功
13. **热启停**：Toggle 开 → spawn + 进 `autoToolNames_`；Toggle 关 → 杀进程树 + 注销工具 + 移出 `autoToolNames_`
14. **孤儿清理**：`fork` 模式退出后，断言子进程也被杀
15. **串行（主 Agent）**：主 Agent 两个 `tools/call` 顺序执行（第二个等第一个返回，`agent.cpp:399-422` 单线程循环），断言无并发在途
16. **shutdownAll**：proJV 退出时全部 server 被杀（进程表中无残留）；验证关闭序列为"关 stdin→等→强杀"而非发送不存在的 `shutdown` 请求
17. **trusted 门**：`trusted=false` 的 server 不 spawn、不注册（断言进程数为 0）
18. **ESC 取消不杀 server**：`cancelTurn()` 后，在途 `callTool` 在 100ms 内返回取消结果，server 收到 `notifications/cancelled`，但**进程仍在、`connState` 仍为 Ready**，随后可再次成功调用
19. **技能隔离**：技能 `config.json` 的 `tools` 里写 `mcp__fake__echo` 时，装载阶段被跳过并打日志；技能子代理的工具表里不含 `mcp__` 前缀工具

`unit/test_mcp.cpp` 必须写进 `tests/CMakeLists.txt` 的 `TEST_SOURCES`（显式列表，不是 glob）。不登记 = 上面 19 条全部零覆盖。`fake_mcp_server` target 同样要在 `tests/CMakeLists.txt` 登记。同时把 `docs/en/dev-guide/module-map.md` 的测试索引补上 `test_mcp.cpp`。

---

## 12. 不做的事

1. **SSE / Streamable HTTP 传输**。stdio 是本版的全部传输；HTTP 要流式解析 + 断线重连，另立迭代。
2. **`tools/list` 分页**。会分页的 server 在本版**完全不可用**。这是明写的交付缺口，不是风险项。
3. **resources / prompts 资源方法**。不在本版范围。
4. **模式 A 下 server 主动请求的应答**（sampling / roots / elicitation）。`capabilities: {}` 声明不支持；不守规矩的 server 记进错误信息。模式 B 下 server 本就不能发请求。
5. **MRTR 多轮交互**。只识别 `resultType: "input_required"` 并回一条明确错误，不做多轮。
6. **per-tool 审批 UI**。只有 `trusted = false` 不 spawn 这一道门。
7. **`IProcessRunner` 改造**。MCP 走 MCPManager 自己的进程管理，runner 一行不改。
8. **自动协议探测**（`server/discover`）。配置必须写死 `protocol`，不做运行时猜测。

---

## 13. 风险表

| 风险 | 触发 | 严重度 | 立刻 | 长期 |
|---|---|---|---|---|
| 注册成功但模型看不见 | 名字没进工具表也没进提示词 | 高 | `autoToolNames_` + 成对断言（提示词无、仍可见） | 工具可见性做成纯函数 + 金测试 |
| Windows 上 stderr 污染 JSON-RPC 流 | server 往 stderr 打日志 | 低（v3 由高降至低） | stderr 独立管道 + 跳坏行兜底（见 6.2） | `--mode=badline` 常驻回归 |
| 业务失败升级成整轮停摆 | `isError:true` 被映射成 `Error:` 前缀 | 高 | 返回 `<server> / <tool>: <text>` | `isError` 改由结构化字段决定 |
| 换工作区后 MCP 仍钉在旧目录 | `saveFullConfig` 重建内置工具，MCP 不跟随 | 高 | 绑定启动工作区 + 日志 + 文档 | 常驻版本里支持热更新 |
| 后台线程写工具表与 Agent 遍历冲突（UB） | F4 在回合中注册/注销 | 高 | Toggle 仅 idle + `syncTools()` 在回合边界 | 单测并发压力 |
| spawn 计数无观测 | 反复 Toggle | 低 | 诊断面板计数 | 并入 F4 面板 |
| `autoToolNames_` 与 `allowedTools_` 互相污染 | 误把 MCP 名塞进 `allowedTools_` | 高 | 两集合独立、`isToolAllowed` 先查 auto | 单测锁死"删提示词行后内置全放行" |
| 技能子代理并发打同一 server | 技能 `config.json` 误写 `mcp__` 工具名 | 高 | 装载时跳过 `mcp__` 前缀并打日志（第 3 节边界） | 若将来要支持，需在 MCPManager 加排队 |
| 误以为 ESC 会关掉 MCP server | 把"取消回合"当成"关停服务" | 中 | 第 6.2 表格明确 ESC 只取消在途调用、不动进程 | 无 |
| **server 进程卡死占住管道** | 假 server 或真 server bug | 中 | 每 server 独立超时 + C2 | 模式 A 心跳 ping（模式 B 无 ping） |
| **孤儿进程残留** | server 自行派生子进程 | 中 | Job Object / killpg 杀整棵树 | 常驻版本里支持热更新 |
| **Toggle 关掉正在执行的调用** | 用户关 server 时调用在途 | 中 | 等超时或返回，不硬杀 | 取消传播给在途调用 |
| `mcp.json` 字段写错 | 手改 JSON | 中 | 逐字段校验 + 逐条日志 | 加 `--check-config` 预检 |
| 2026-07-28 新协议 server 无法连接 | 只实现 stateful | 中 | 配置 `protocol = "stateless"` 显式声明 | 跟随协议演进 |

---

## 14. 实施切分

| 刀 | 内容 | 完成标志 |
|---|---|---|
| M1a | 进程与分帧：CreateProcess/Job Object + fork/exec/setpgid、换行分帧、跳坏行、关闭序列（关 stdin→等→强杀）、孤儿清理、线程归属（F4 动作用后台线程） | `badline` / `noexit` / `hang` / `fork` 四例通过 |
| M1b | 双时代握手与注册：模式 A `initialize` + `notifications/initialized`；模式 B 每请求 `_meta` 三件套 + `resultType`；`tools/list`、`inputSchemaJson`、`registerToolIfAbsent`、`autoToolNames_` + `isToolAllowed`、成对断言 | 提示词无 MCP 名仍可见、删工具行后内置仍可用、stateful/stateless 各一例、`badmeta` 一例通过 |
| M1c | 调用与生命周期：`tools/call` 透传 + 软错误映射 + `input_required` 识别 + dead 标记 + 取消链路（ESC→notifications/cancelled）+ `trusted` / `protocol` 配置 + shutdownAll | `softfail` / `inputreq` / 取消三例通过；shutdownAll 无残留进程；`trusted=false` 零进程 |
| M1d | UI 与注入：F4 面板 + Toggle 热启停 + `injectMcpContext()` + 底部提示行加 `F4 mcp` | 手工验收：F4 开/关 server，模型能看到/看不到对应工具；TUI 不卡顿 |
| M1e | 文档转立场：第 15 节的 6 处（双语 12 文件）+ 新增 MCP 用户文档（`docs/en` 与 `docs/zh` 的 `user-guide/`）+ `08-reference.md` 的 F 键表补 F4 + `module-map.md` 补 `mcp_manager` | 第 15 节 6 处全部改写；`docs/en` 与 `docs/zh` 一一对应；本 TEMP 删除 |
| M2 | pytool 改走 `autoToolNames_`（独立一刀，MCP 之后）：注册时也加入自动可见集合，免去手写提示词 | 新建一个 pytool 不写提示词也能被模型调用；删提示词行后内置工具仍全放行 |

**验收**：`ctest` 全绿且 MCP 新增用例 ≥ 19，M2 新增用例 ≥ 2；故意让 `badline` 用例红一次，证明 `test_mcp.cpp` 真在跑；第 15 节 6 处声明全部改写完毕（`grep -ri "add an MCP layer" AGENTS.md CONTRIBUTING.md CONTRIBUTING_zh.md` 零命中、`grep -ri "no MCP" README.md README_zh.md` 零命中）；`docs/en` 与 `docs/zh` 一一对应。

---

## 15. 声明改写：6 处，不是 1 处（v3 重写）

**v2 只改了 `AGENTS.md`，漏了 5 处。** "没有 MCP"这个立场在当前仓库里写在 **6 个地方**，按双语规矩各一份 = **12 个文件**：

| 文件:行 | 现有内容 | 性质 | 怎么改 |
|---|---|---|---|
| `AGENTS.md:36` | `Do not add an MCP layer, a plugin registry or a package manager.` | 禁令 | 重写为"扩展仍是文件 + MCP server 是外部进程" |
| `CONTRIBUTING.md:56` | `Adding an MCP layer, a plugin registry, or a package manager.` | 不接受清单 | 从"不接受"移出，改为"MCP 走 `mcp.json` 声明" |
| `CONTRIBUTING_zh.md:49` | `引入 MCP 层、插件注册表或包管理器。` | 同上 | 同上，与英文同步 |
| `README.md:106` / `README_zh.md:103` | 对比表里 `MCP servers, hooks` 这一格 | 定位对比 | 该格改为"内置"，`hooks` 留在未做列 |
| `README.md:133` / `README_zh.md:127` | 整段 `**No MCP.**` | **产品主张** | 不能删，要换论点，见 15.2 |

### 15.1 AGENTS.md（禁令 → 边界）

改写为（保留本意：二进制不加载代码、不要注册表、不要包管理器）：

> Every extension is a file: skills, roles, themes, pytool folders, prompt files, and MCP servers declared in `mcp.json`. MCP servers are external processes proJV talks to over stdio JSON-RPC; they are never loaded as code, never registered through a plugin registry, and never installed through a package manager. No plugin registry, no package manager, and nothing the binary loads as code.

### 15.2 README 那段"没有 MCP"（主张 → 新论点）

现状（`README.md:133` / `README_zh.md:127`）：

> **No MCP.** A tool here is a struct with a JSON schema and a function. Any MCP capability can be written in a few dozen lines as a pytool or a builtin tool; no protocol layer is needed.

**这段不能简单删掉。** 它不只是禁令，它在讲一个设计主张："工具就是结构体加函数，MCP 能力几十行就写完了"。直接删等于放弃论证，而这句话本身**仍然成立**——新增的只是"外部常驻进程也能当工具"。

改写方向（中英同一口径）：

> **MCP is a config line, not a plugin system.** A built-in tool is still a struct with a JSON schema and a function, and a pytool is still a file in a folder. proJV adds a third way to get a tool: point `projv_files/mcp.json` at an external process and talk to it over stdio JSON-RPC the same way `exec_shell` talks to a command -- except the process stays alive between calls. Nothing is loaded as code, no registry is consulted, no package manager is involved. If a few dozen lines of pytool do the job, write the pytool; reach for MCP when the tool already exists as an MCP server.

### 15.3 不放松的部分

以下判断在 v3 里**一条都不放松**，用户文档照原样写：

- 二进制依然**不加载任何代码**：MCP server 是配置声明的外部进程，不是动态库、不是插件；
- 依然**没有插件注册表、没有包管理器**；
- 技能、角色、主题、pytool、提示词仍然是"扔文件"。

唯一新增：**`mcp.json` 里声明的外部进程**也属于"文件即扩展"。

### 15.4 合并顺序

这一刀放在 **M1d 验收通过之后、发版之前**合并。理由：声明先改成"支持 MCP"而功能未完成，用户会撞上"官方说支持但用不了"的空白功能；功能先合并而声明后改，仓库里就会出现"代码有 MCP、文档说没有"的不一致窗口。两边都不好，所以放在功能验收之后、tag 之前。

---

## 16. 附录 A：官方规范定位（实现前必读）

规范仓库：`https://github.com/modelcontextprotocol/modelcontextprotocol`
原始文件（`docs/specification/` 下）：

| 用途 | 文件 |
|---|---|
| legacy 生命周期（initialize/initialized） | `2025-06-18/basic/lifecycle.mdx` |
| legacy stdio 传输 | `2025-06-18/basic/transports.mdx` |
| legacy tools（list/call 返回结构） | `2025-06-18/server/tools.mdx` |
| 2025-11-25 基线（模式 A 使用的 protocolVersion） | `2025-11-25/basic/lifecycle.mdx` |
| modern 变更摘要 | `2026-07-28/changelog.mdx` |
| modern 版本协商与 `_meta` | `2026-07-28/basic/versioning.mdx`、`2026-07-28/basic/index.mdx` |
| modern stdio 传输 | `2026-07-28/basic/transports/stdio.mdx` |
| modern tools（同名文件，内容已变） | `2026-07-28/server/tools.mdx` |

线上页面：`https://modelcontextprotocol.io/specification/<版本>/...`

**已知关键差异（实现时最容易踩）**：

1. **不存在 `shutdown` 请求方法**。关闭靠关 stdin。见 6.2 关闭序列。
2. **2026-07-28 不是"只去掉 HTTP session"**：它去掉了**协议级握手**，改为每请求带 `_meta`；`2025-11-25` 仍是 legacy 握手版本。
3. **2026-07-28 的 Roots / Sampling / Logging 已 deprecated**；`ping`、`logging/setLevel`、`notifications/roots/list_changed` 已删除。
4. **2026-07-28 server 禁止向 stdout 写请求**（stdio 原文）；只能通过 `InputRequiredResult` 间接请求额外输入。
5. **所有 result 必须带 `resultType`**（`complete` / `input_required`）；老 server 省略时按 `complete` 处理。

---

## 17. 附录 B：新增/改动的 API 清单（实现者照着建）

### 17.1 新增文件

| 文件 | 内容 |
|---|---|
| `src/core/mcp_manager.h/.cpp` | `McpManager`、`McpServerHandle`、`McpTransport`（stdio 管道读写） |
| `src/core/mcp_config.h/.cpp` | `mcp.json` 解析与校验（可并入 mcp_manager） |
| `src/core/mcp_protocol.h/.cpp` | JSON-RPC 编解码、分帧、`_meta` 组装、错误映射 |
| `src/core/mcp_process.h/.cpp` | Windows Job Object / Linux pgid 进程控制（可并入 mcp_manager） |
| `tests/unit/test_mcp.cpp` | 上述测试 |
| `tests/fakes/fake_mcp_server.cpp` | 假 server（12 模式，独立可执行） |

### 17.2 改动的现有文件

| 文件 | 改动 |
|---|---|
| `src/client/models.h` | `ToolDefinition` 加 `std::string inputSchemaJson;` |
| `src/client/models.cpp` | `toJsonSchema()` 首行：`if (!inputSchemaJson.empty()) return parse(inputSchemaJson);` |
| `src/tools/registry.h/.cpp` | 加 `bool registerToolIfAbsent(def, exec)`、`size_t unregisterByNamePrefix(prefix)` |
| `src/core/agent.h/.cpp` | 加 `std::set<std::string> autoToolNames_`、`addAutoTools/removeAutoTools`、`isToolAllowed()`；改 `getFilteredToolDefinitions()` 与 `executeTool()` |
| `src/tui/app_tui.h/.cpp` | 持 `std::unique_ptr<McpManager>`；`initialize()` 里先 `ensureConfigFile()` 再 `loadFromJson()` 再启动；`shutdown()` 调 `shutdownAll()`；新增 `injectMcpContext()` |
| `src/tui/main_tui.cpp` | F4 事件 → 打开 MCP 面板（参照 `config_view.cpp` 的模态写法）；**底部提示行 `:596-603` 插入 `hint(keycap("F4"), "mcp")`**（该行已排 12 项，注意换行）；`shutdown` backstop 加 MCP 收尾 |
| `.gitignore` | 加一行 `mcp.json`（与 `config.toml` 同款写法，`:33`） |
| `tests/CMakeLists.txt` | `TEST_SOURCES` 加 `unit/test_mcp.cpp`；**新增 `fake_mcp_server` target** 并 `target_compile_definitions(... FAKE_MCP_SERVER_PATH="$<TARGET_FILE:fake_mcp_server>")`；**必须加 `add_dependencies(proJV_tests fake_mcp_server)`**，否则干净构建时宏指向的文件还不存在 |
| `AGENTS.md` + `CONTRIBUTING.md` + `CONTRIBUTING_zh.md` + `README.md` + `README_zh.md` | 按第 15 节的 **6 处**改写（不是 1 处） |
| `docs/en/user-guide/08-reference.md` + `docs/zh/` 同名 | F 键表补一行 `F4`（MCP servers）；补 `mcp.json` 字段说明 |
| `docs/en/dev-guide/module-map.md` + `docs/zh/` 同名 | 模块表补 `core/mcp_*`；测试索引补 `test_mcp.cpp` |

### 17.3 `McpManager` 公开接口（建议签名）

```cpp
class McpManager {
  public:
    // 若 mcp.json 不存在则写入 {"servers": []}（镜像 ensureDefaultPrompts）
    static bool ensureConfigFile(const std::string& path);
    bool loadFromJson(const std::string& path, std::string& err);
    // 后台线程执行；立即返回。UI 通过 connState() 轮询。
    // 线程由 McpServerHandle::worker_ 持有：start() 先 join 旧的再起新的，
    // stop() 先置 cancel 再 join。禁止 detach（见 6.2「后台线程归谁管」）。
    // 注意：ESC 取消回合不调用这里——只置 callTool 的在途取消标志。
    void start(const std::string& serverName);
    void stop(const std::string& serverName);
    void restart(const std::string& serverName);
    void shutdownAll();   // 逐个 stop()，即逐个 join
    ~McpManager();        // 内部调 shutdownAll()，兜住忘记调的退出路径

    ConnState connState(const std::string& serverName) const;   // 原子读
    std::string buildContextSnippet() const;                     // 只列 Ready

    // 注册进 ToolRegistry 并把名字加入 Agent::autoToolNames_
    void syncTools(ToolRegistry& registry, Agent& agent);   // 仅 Agent 线程调用

    // 由 registry 的执行器回调进来（executor lambda 在 TuiApp 里创建，见 6.2）。
    // cancelPoll：每 100ms 调一次，true 即写 notifications/cancelled 并返回取消结果。
    // 传 lambda 而不是让 McpManager 认识 ToolRegistry，避免多一条依赖。
    std::string callTool(const std::string& serverName, const std::string& toolName,
                         const std::string& argsJson,
                         const std::function<bool()>& cancelPoll);
};

// McpManager 内部的 handle 形状（v3 补全）
struct McpServerHandle {
    McpConfig cfg;                       // name/command/args/env/protocol/trusted/timeout_ms
    std::thread worker_;                 // start/stop/shutdownAll 三个同步点负责 join
    std::atomic<ConnState> connState{ConnState::Stopped};
    std::atomic<bool> cancelRequested{false};
    std::vector<ToolDefinition> tools;   // 本 server 的最新快照，只由 worker 写
    // 平台句柄：pid + jobObject（Win）| pgid（Linux）+ 三根管道
    ProcessHandles proc;
};
```

### 17.4 模式 A 握手报文（2025-11-25）

```json
→ {"jsonrpc":"2.0","id":1,"method":"initialize","params":{
     "protocolVersion":"2025-11-25",
     "capabilities":{},
     "clientInfo":{"name":"proJV","version":"<PROJV_VERSION>"}}}
← {"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2025-11-25",
     "capabilities":{"tools":{}},"serverInfo":{"name":"fs","version":"1.0"}}}
→ {"jsonrpc":"2.0","method":"notifications/initialized"}
```

协商规则（`2025-06-18/basic/lifecycle.mdx`）：若 server 回一个 client **不认识**的 `protocolVersion`，client 必须断开；若 client 支持该版本则继续。

### 17.5 模式 B 请求（2026-07-28）

```json
→ {"jsonrpc":"2.0","id":1,"method":"tools/list","params":{
     "_meta":{
       "io.modelcontextprotocol/protocolVersion":"2026-07-28",
       "io.modelcontextprotocol/clientCapabilities":{},
       "io.modelcontextprotocol/clientInfo":{"name":"proJV","version":"<PROJV_VERSION>"}
     }}}
← {"jsonrpc":"2.0","id":1,"result":{
     "resultType":"complete",
     "ttlMs":60000,
     "cacheScope":"private",
     "tools":[{"name":"read_file","description":"...","inputSchema":{"type":"object"}}]}}
```

### 17.6 `tools/call` 结果解析（两模式通用）

```json
{"jsonrpc":"2.0","id":2,"result":{
   "resultType":"complete",           // 模式 B 才有；模式 A 无此字段
   "content":[
     {"type":"text","text":"file contents..."},
     {"type":"image","data":"<base64>","mimeType":"image/png"}
   ],
   "structuredContent": {...},        // 任意 JSON 值，可为 object/array/string
   "isError": false}}
```

聚合规则：把所有 `content[]` 里 `type=text` 的 `text` 用 `\n` 拼接作为工具结果字符串；`type=image/audio/resource/resource_link` 无文本表示 → 追加一行 `[image: <mimeType>]` 之类的占位，不下发二进制（终端显示不了）。`isError=true` 仍返回文本、但**不加 `Error:` 前缀**（见 6.3 软错误）。

> **关键坑：`Error:` 前缀是项目判定工具失败的硬约定。** `agent.cpp:827` 用 `r.content.compare(0, 6, "Error:") == 0` 来设置 `isError`。所以：
> - MCP 返回的**业务错误文本绝不能以 `Error:` 开头**（否则被误判为工具失败，连调两次会触发 `ERROR_CONSECUTIVE_FAILURES` 关掉整轮工具能力）。统一格式 `<server> / <tool>: <text>`。
> - MCPManager 自己产生的**真错误**（超时、server dead、`input_required`）则应显式以 `Error:` 开头，让 Agent 正确标记 `isError`。

---

## 18. 附录 C：交给实现 Agent 的执行须知

1. **先读规范原文再写代码**：附录 A 的路径是唯一事实来源；本文件第 4 节的报文样例可直接对照。
2. **先读这些代码再动手**（按依赖顺序）：
   `src/tools/registry.h` → `src/client/models.h`（ToolDefinition/toJsonSchema）→ `src/core/agent.cpp`（`getFilteredToolDefinitions` 在 931 行附近、`executeTool` 在 819 行附近）→ `src/platform/process_runner_win.cpp`（进程控制可参照，但不复用）→ `src/tui/app_tui.cpp`（`setupTools`/`initialize`/`injectWorkspaceContext`/`shutdown`）→ `src/tui/main_tui.cpp`（F 键与 backstop）。
3. **不要改 `IProcessRunner`**；不要改 `external/`；颜色只走主题系统。
4. **每刀完成就跑** `ctest --test-dir build -C Release --output-on-failure`，红的不许进下一刀。
5. **新增测试文件必须手工进 `tests/CMakeLists.txt` 的 `TEST_SOURCES`**。
6. **中英双语文档**：实现完成后 `docs/en/`、`docs/zh/` 各补一份正式文档，本 TEMP 删除。
7. **版本只改 `CMakeLists.txt` 的 `project() VERSION`**，clientInfo 里的版本从 `PROJV_VERSION` 宏读。
8. **遇到规范与本文冲突**：以规范为准，并回来更新本文。

---

## 19. v3 修订记录

对照 `src/` 逐条复核后，v2 → v3 的改动如下。**v2 的机制骨架全部保留**，改动集中在"缺口补全"与"无效内容订正"两类。

| # | 位置 | v2 的问题 | v3 的处理 | 等级 |
|---|---|---|---|---|
| 1 | §6.2 | 只写"start/stop 跑在后台线程"，未定线程归属 → 退出/restart 时 use-after-free | 新增「后台线程归谁管」：每个 handle 一个 `worker_`，三个同步点 join，禁止 detach | **阻塞实现** |
| 2 | §6.2 | 写"MCPManager 注册 cancel hook"，`ToolRegistry` 无此扩展点 | 改为轮询 `ToolRegistry::isCancelled()`（`registry.h:23` 已是 public）+ 传 `cancelPoll` 回调，registry 一行不改 | **阻塞实现** |
| 3 | §6.2 / §6.3 | 默认继承 runner 的 stderr 并流，靠 C3 兜底 | MCPManager 自建三根管道，stderr 独立 drain 进日志；C3 降级为防坏 server；补 `CREATE_NO_WINDOW` / Job Object / 不包 shell 三条对照 | 高风险消除 |
| 4 | §8 | 未说明 `mcp.json` 在哪儿 | 补：在 **exe 目录**（`config.cpp:36-38`），安装级全局；开关全局、cwd 随启动工作区，两条并排写进用户文档 | 用户必问 |
| 5 | §11.1/§11.2 | `dup` 用例恒真（注册名恒带 `mcp__` 前缀，撞不上内置名） | 改为验"两个同名 server 只 spawn 一个"；`registerToolIfAbsent` 保留为代码防御 | 无效用例 |
| 6 | §15 | 只改 `AGENTS.md`，漏 5 处 | 扩成 6 处、双语 12 文件；README 那段"没有 MCP"给新论点而非删除；补 15.4 合并顺序 | 立场缺口 |
| 7 | §14 | 无文档刀 | 新增 M1e（文档转立场 + 用户文档 + F 键表 + 模块表） | 交付完整度 |
| 8 | §17.2 | 漏 `main_tui.cpp` 提示行、`add_dependencies`、docs 双语 | 补齐 4 项 | 实现细节 |
| 9 | §2 / §3 | 行号漂移 3 处 | `256/900/962`、`851`；另补 3 条已核实事实（`agent->run()` 唯一调用点、`Event::F4` 存在、提示行已满） | 事实准确度 |

**复核确认无需改动的部分**（v2 判断正确，实现者可直接照做）：

- `autoToolNames_` 与 `allowedTools_` 正交：`allowedTools_` 空 = 全放行的逃生口语义保持不变（`agent.cpp:962`）；
- `syncTools()` 只在回合边界写工具表：`agent->run()` 全仓库**唯一**调用点在 `app_tui.cpp:528`，覆盖完整；
- `inputSchemaJson` 原样透传：`toJsonSchema()` 短路两行即可（`models.cpp:7`）；
- 注册期必须校验 schema：`to_json(ToolDefinition)` 的 catch 会静默回落 `{"type":"object"}`（`models.cpp:196-203`），不校验就等于让模型自己编参数名；
- 技能不接 MCP：`skill_runner.cpp:393-394` 按名查表 + `sub_agent.cpp:186-189` 白名单，两处都成立，跳过 `mcp__` 前缀这条边界可行。
