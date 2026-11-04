# TEMP 设计稿：proJV 作为 MCP Client

> **TEMP（临时设计稿）** —— 中文、仅供评审，未定稿。
> 不进 `docs/en/` 镜像；定稿后按 `AGENTS.md` 拆成双语正式文档，本文件删除。
> 本文只做设计，不含实现。行号来自实际读取；未核实标 `[待证]`。

---

## 0. 结论与范围

**有条件可行**。M1 只做 **stdio 传输 + 工具注册 + 工具调用**，并且**proJV 只做 client，不做 server**。

三条硬边界：

1. **每次调用起一个新进程**，`IProcessRunner` 一行不改
2. **任意 JSON Schema 原样透传**，不降级、不扩 `ToolParameter`
3. **MCP 工具永远走角色门**，不借"空 allowlist = 全放行"那条逃生口

M1 明确不做：常驻连接、`tools/list` 分页、SSE/HTTP、热加载、per-tool 审批 UI、server 主动请求的应答。

---

## 1. 现状事实

| 事实 | 证据 |
|---|---|
| 执行器收的是原始 JSON 字符串 | `registry.h:9` `ToolExecutor = function<string(const string&)>` |
| 工具注册是 map 赋值，**重名静默覆盖** | `registry.cpp:5-8` `definitions[def.name] = def; executors[def.name] = executor;` |
| `ToolParameter` 只有四种 type | `models.h`：string / number / boolean / array |
| 工具 schema 由 parameters 拼出来 | `models.cpp:7-38` `toJsonSchema()`，异常回落 `{"type":"object"}` |
| **发给模型的工具表按白名单过滤** | `agent.cpp:931` `req.tools = getFilteredToolDefinitions();`，`:959-964` 空则全放行、否则按名单过滤 |
| 空白名单 = 不设限 | 同上，`if (allowedTools_.empty()) return all;` |
| 进程管理器一次一个子进程、跑完才算完 | `iprocess_runner.h`，`Run()` 持锁到子进程退出 |
| 默认超时 **30 分钟** | `iprocess_runner.h:12` `kProcessTimeoutMs = 1800000` |
| **Windows 上子进程 stderr 塞进 stdout 同一根管道** | `iprocess_runner.h:25-27` 注释；`process_runner_win.cpp:80` `si.hStdError = hOutWr;` |
| 技能子代理的白名单是真闸门 | `sub_agent.cpp:186-189` 逐个比对；`skill_runner.cpp:393-394` 按同名单注册 schema |
| 项目明文禁止加 MCP 层 | `AGENTS.md`：`Do not add an MCP layer, a plugin registry or a package manager.` |

---

## 2. 关键机制：两层工具门

这是整篇设计里最重要的一段。**把工具注册进去，不等于模型看得见它。**

`agent.cpp:931` 决定发给模型的工具表，而它按 `allowedTools_`（= 提示词里那行 `tools available:`）过滤。所以一个注册成功的 MCP 工具，如果名字没进白名单：不报错、构建绿、测试绿，**模型根本不知道它存在**，也就永远不会调。

所以必须有第二层判断。`allowedTools_` 保持只装 `parseAvailableTools(systemPrompt_)`，**不做并集**：

```cpp
bool Agent::isToolAllowed(const std::string& n) const {
    if (mcpToolNames_.count(n)) {                 // MCP：只认角色门
        auto it = mcpToolsByRole_.find(currentRole_);
        return it != mcpToolsByRole_.end() && it->second.count(n) != 0;
    }
    return allowedTools_.empty() ||               // 内置：语义一字不动
           std::find(allowedTools_.begin(), allowedTools_.end(), n) != allowedTools_.end();
}
```

- `getFilteredToolDefinitions()`（`:959-964`）的快速返回改成 `allowedTools_.empty() && mcpToolNames_.empty()`，其余走 `isToolAllowed`
- `executeTool()`（`:819-820`）的判定换成 `if (!isToolAllowed(call.name))`
- **查询时求值，不缓存**：`setSystemPrompt` / `replaceSystemPrompt` 有**三个**调用点（`app_tui.cpp:260`、`:902`、`:964`），缓存等于让"凡改提示词处都得记得重算"靠纪律维持

不变式：用户手改提示词、删掉 `tools available:` 整行 → 内置工具照旧全放行（老语义），**MCP 工具仍被角色门挡住**。

---

## 3. 协议范围：client 就够，但要理解"client"不是单口相声

### 3.1 澄清一个容易误解的点

MCP 是**双向**协议，两边都能主动发请求：

- client → server：`initialize`、`tools/list`、`tools/call`
- server → client：`sampling/createMessage`、`roots/list`、`elicitation`

后面那组看着像"server 的活"，容易被误读成"要顺便实现 server"。**不是。**

**做法**：`initialize` 里声明 `capabilities: {}`，即"我不支持任何额外能力"。规矩的 server 就不会来借模型、问目录。proJV 没有任何理由在 M1 声明 `sampling` —— 那等于把自己的模型借给外部进程用。

于是：**这条限制不是"做不到"，是"不声明就不欠它"。** 真遇到不守规矩的 server 发来请求，一次性模型无法应答，处理办法是把它记进错误信息（见 6.3），而不是假装能回。

### 3.2 M1 必须支持的方法

| 方法 | 用途 |
|---|---|
| `initialize` | 握手，声明 `capabilities: {}`、`clientInfo`、`protocolVersion` |
| `notifications/initialized` | 握手第二步（无 id，不等回复） |
| `tools/list` | 拿工具清单 + 每个工具的 `inputSchema` |
| `tools/call` | 调工具，取 `result.content[0].text` |

通知类消息（`progress` / `log`）读掉丢弃即可，不处理。

### 3.3 分帧

**换行分隔 JSON**（不是 LSP 的 `Content-Length`）。`nlohmann::json::dump()` 不产裸换行，出站天然安全；入站按 `\n` 切，单行上限 1 MiB，超限判违约并断开。

### 3.4 明确排除

- **SSE / HTTP**：要流式解析 + 断线重连 + session id，失败模式与 stdio 完全不同
- **`tools/list` 分页**：`nextCursor` 非空则**整台拒绝注册**。静默只注册第一页会让模型对缺失的工具名反复猜，比报错贵得多。**这是 M1 的明写缺口**，见第 12 节。

---

## 4. Schema：原样透传

`ToolDefinition` 加一个字段：

```cpp
std::string inputSchemaJson;   // 非空则 toJsonSchema() 原样发出
```

`toJsonSchema()`（`models.cpp:7`）首行短路即可，约两行。

**为什么不降级**：有人会想把 MCP 的嵌套 object 降成 `type: "string"` 让模型自己拼 JSON。不成立——`ToolExecutor` 收的本来就是原始 JSON 字符串，`ToolParameter` 只决定模型看到的形状，**从不参与传输**。透传比写降级逻辑还省事。

降级的实际后果：模型把 `{"a":{"b":1}}` 写成 `"{\"a\":{\"b\":1}}"`（带双重转义）塞进 string 字段，server 的 schema 要 object，回 `invalid params`，而错误文本在 server 侧。

**注册期校验**：`parse` 一次 `inputSchema`，不是 object 或解析失败 → **只拒这一个工具**并记一行。否则 `to_json` 每次构建请求都会回落成 `{"type":"object"}`，properties 与 required 全丢，模型自己编参数名。

---

## 5. 命名与撞名

- 注册名 `mcp__<server>__<tool>`，两侧只留 `[A-Za-z0-9_-]`，其余替换 `_`，总长截 64
- **不用冒号**：提示词那行已经用 `:` 当标记，名字里再出现冒号只会让以后排查解析变成猜谜（`tool_list_parser.cpp` 按 `,` 切，技术上不破坏，但没有好处）
- 走新增的 `registerToolIfAbsent`：**内置永远优先**，撞名返回 false + 一行 warn
- sanitize 后折叠成同名也算撞名（`a+b_c` 与 `a_b+c` 会撞）
- MCP 工具名**不写进任何提示词的工具行**，走角色门；这也避免动到 `parseAvailableTools` 那条脆弱的解析

---

## 6. 生命周期：一次性 spawn，不碰 IProcessRunner

### 6.1 为什么不常驻

MCP 的常规形态是长驻双向连接，但它和 proJV 现有的进程模型直接冲突：

- `iprocess_runner.h` 明写全进程一个 runner，`Run()` 持锁到子进程结束，`Cancel()` 指"当前那个进程"
- 常驻连接要么永远持锁（把 `exec_shell` 和所有 skill worker 堵死），要么**重写一套 CreateProcess/Job Object 与 fork/setpgid/killpg**，而那段杀进程树的代码全仓库只有一份、是对的，写第二份必然漂移

**这不违反"不许把 runner 改成池"**：那条保护的是 shell / pytool 的一次性串行模型，MCP 没动它。

### 6.2 每次调用的形状

spawn → 依次写 `initialize`、`notifications/initialized`、`tools/call`（换行分隔）→ 关 stdin → 读 stdout 到 EOF → 解析 → 杀进程树。`tools/list` 同理，在启动时跑一次。

### 6.3 四条硬条件

| # | 条件 | 不做的后果 |
|---|---|---|
| C1 | 用**自己的**超时（默认 120s），绝不用 `kProcessTimeoutMs`（30 分钟） | 一个挂住的 server 占着 runner 全局锁 30 分钟，`exec_shell` 与所有 skill worker 排队 |
| C2 | server 收到 EOF 不保证退出；超时 `Cancel()` 后若 stdout 里已有 id 匹配且可解析的响应 → **按成功计**并记一行 warn | 有响应但慢的 server 被当成失败，工具结果丢失 |
| C3 | **跳过解析不了的行与不认识的 id，绝不中止** | Windows 上 stderr 混进 stdout（第 1 节），Linux CI 全绿、Windows 全坏 |
| C4 | `tools/list` 的 `nextCursor` 非空 → **整台拒绝注册** | 静默只注册半套工具，模型对缺失的工具名反复猜 |

另两条：

- 首次调用失败即标记该 server **dead**，后续返回一行稳定错误、不再 spawn（否则连续两次业务失败会触发 `ERROR_CONSECUTIVE_FAILURES`，关掉整轮工具能力）
- 超时错误里**点名** `stdout` 中带 `method` 的未应答行，例如 `MCP server 'x' sent a request we cannot answer in one-shot mode: sampling/createMessage`。只写 "did not answer" 会让运维永远猜不到是协议能力缺口

### 6.4 WorkDir

注册时绑定**启动时的工作区**，打一行 `[mcp] server 'fs' workDir=<path>`。配置里**不提供 `cwd` 键**（少一个旋钮、少一条优先级规则），文档写死"换工作区必须重启"。

理由：`app_tui.cpp:853` 的 `saveFullConfig` 会重建内置工具以跟随新工作区，MCP 进程不会。不写清楚就会出现"以为在 B 项目里写文件，实际写进 A 项目"。

---

## 7. 安全：fail closed

- `trusted` 默认 **false** → **不 spawn、不注册**，启动日志一行 `[mcp] skip 'x' (trusted=false)`
- `trusted = true` 是唯一旋钮；`roles` 决定给哪些角色
- **不做 per-tool 审批 UI**：那要动 `approval_logic.*` + TUI，M1 的收益不抵
- `validateToolPath` 只约束内置文件工具；**MCP 工具的副作用面完全由 `trusted` + `roles` 两个配置键收窄**，没有第二道运行时闸。这点必须写进用户文档

---

## 8. 配置：仍然是"一个文件"

```toml
[[mcp.servers]]
name = "fs"
command = "npx"
args = ["-y", "@modelcontextprotocol/server-filesystem", "."]
env = { TOKEN = "..." }
trusted = true
roles = ["executor"]
timeout_ms = 120000
```

**选 `config.toml` 而不是独立 `mcp.json`**：

- `ProcessConfig` 带 `extraEnv`，真 server 基本都要 TOKEN，而 `config.toml` 在 `.gitignore` 里就归类为 `# Config with secrets`
- 多一个文件就多一套忽略规则、一套文档、一套迁移说明

**`roles` 必须校验**：合法角色 = `ensureDefaultPrompts()` 过滤掉 `compactor.md` 之后的文件名（不含 `.md`）。不存在的名字**逐条打日志**并让该 server 整台不注册：

```
[mcp] server 'fs': role 'executor' has no prompt file (have: analyzer, coder, designer)
```

**不热加载**。改配置重启生效；运行中换 map 是新的竞态面。

**注意**：MCP 工具可以被写进 `skills/*/config.json` 的 `tools`，从而绕开 `roles` 门（技能子代理用文件名白名单，不读角色）。这不是漏洞，但必须写进文档——它是唯一一条绕开角色门的路径。

---

## 9. 测试

不联网、不依赖 python、不新增 target：`projv_tests` 自带假 server。

- `main()` **第一行**（任何输出、`doctest::Context` 之前）判 `argv[1] == "--fake-server"` 并接管 stdin/stdout 循环。晚一步，doctest 的 banner 就会污染 JSON-RPC 帧。
- 命令行拼装时**必须给可执行路径加引号**：CI 的 `tests` 路径常含空格，不加会静默非 0 退出，表现成"所有 MCP 用例都报 server 没回应"。
- 模式：`ok` / `dup`（声明一个叫 `read_file` 的工具）/ `badline` / `noexit`（有响应但不退出）/ `softfail`（`isError:true`）/ `hang`（一条都不回）。超时在测试里覆盖成 1s。
- 握手缺 `protocolVersion` / `capabilities` / `clientInfo` 任一项，假 server 直接拒绝——把"真 server 会拒"的失败挪进树内。

必须覆盖：

1. 注册：`tools/list` → `mcp__fake__echo` 出现在工具表
2. 可见性：`coder` 角色看得见它，`analyzer` 角色看不见（**成对断言**，只测一半会放行一个全空的功能）
3. 逃生口：提示词删掉 `tools available:` 整行后，`read_file` 仍可用而 MCP 工具不可用
4. 坏行：`badline` 模式下正常行照常解析，进程不中止
5. 不退出：`noexit` 模式下按 C2 判成功、不卡满超时
6. 挂死：`hang` 模式下 1s 超时返回错误，进程树被杀
7. 软错误：`softfail` 连调两次，两次结果 `isError` 均为 false、首 6 字符不是 `Error:`
8. 撞名：`dup` 模式下内置 `read_file` 仍在，MCP 侧被拒并 warn

`unit/test_mcp.cpp` 必须写进 `tests/CMakeLists.txt` 的 `TEST_SOURCES`（显式列表，不是 glob）。不登记 = 上面 8 条全部零覆盖，文件躺在磁盘上、CI 全绿。

---

## 10. AGENTS.md 改写

现状：

> Do not add an MCP layer, a plugin registry or a package manager. Every extension is a file: skills, roles, themes, pytool folders, prompt files.

改写为（保留本意：二进制不加载代码、不要注册表、不要包管理器）：

> Every extension is a file: skills, roles, themes, pytool folders, prompt files, and MCP servers declared in `config.toml`. No plugin registry, no package manager, and nothing the binary loads as code.

---

## 11. 风险表

| 风险 | 触发 | 严重度 | 立刻 | 长期 |
|---|---|---|---|---|
| 注册成功但模型看不见 | 名字没进工具行也没进角色门 | 高 | `isToolAllowed()` 两层门 + 成对断言 | 工具可见性做成纯函数 + 金测试 |
| Windows 上 stderr 污染 JSON-RPC 流 | server 往 stderr 打日志 | 高 | 跳坏行与陌生 id | `--mode=badline` 常驻回归 |
| 业务失败升级成整轮停摆 | `isError:true` 被映射成 `Error:` 前缀 | 高 | 返回 `<server> / <tool>: <text>` | `isError` 改由结构化字段决定 |
| 换工作区后 MCP 仍钉在旧目录 | `saveFullConfig` 重建内置工具，MCP 不跟随 | 高 | 绑定启动工作区 + 日志 + 文档 | 常驻版本里支持热更新 |
| 每保存一次配置就重 spawn 全部 server | 注册长在 `setupTools()` 里 | 高 | 注册移出 + 幂等闸 | spawn 计数进诊断面板 |
| `roles` 写错名字 → 零工具零日志 | 照抄示例 | 中 | 对现存 prompt 校验 + 逐条日志 | 加 `--check-config` 预检 |
| 有状态 server 用不了 | 浏览器 / playwright 类 | 中 | 文档标注 | 常驻连接（另立迭代） |
| 慢 server 占全局锁 | skill 并行 + server 卡住 | 中 | 120s 超时兜底 | 独立连接管理器 |
| 提示词文件重命名 → 角色门失配 | 用户改 `coder.md` | 中 | fail closed + 打印角色↔工具映射 | 角色身份改成显式配置键 |

---

## 12. 不做的事（读者容易误以为已包含）

1. **常驻连接与 server 主动请求的应答**。一次性模型没有读循环，结构上做不到；留下的是诊断（超时错误里点名 method），不是功能。
2. **`tools/list` 分页**。会分页的 server 在 M1 里**完全不可用**。这是明写的交付缺口，不是风险项。
3. **有状态 server**。每次调用新连接，浏览器 / playwright 这类不支持。
4. **per-tool 审批 UI**。只有 `trusted = false` 不 spawn 这一道门。
5. **热加载、SSE/HTTP、resources/prompts/sampling**。

---

## 13. 实施切分

| 刀 | 内容 | 完成标志 |
|---|---|---|
| M1a | 传输与死法：换行分帧、跳坏行、120s、EOF 判定、杀进程树、假 server 六模式 | `badline` / `noexit` / `hang` 三例通过 |
| M1b | 注册与可见性：`initialize` + `tools/list` + `inputSchemaJson` + `registerToolIfAbsent` + `isToolAllowed` + 成对断言 | `coder` 含、`analyzer` 不含、删工具行后内置仍可用 |
| M1c | 调用：`tools/call` 透传 + 软错误映射 + dead 标记 + `trusted` / `roles` 配置 | `softfail` 连调两次不触发连续失败保护 |

**验收**：`ctest` 全绿且新增用例 ≥ 8；故意让 `badline` 用例红一次，证明 `test_mcp.cpp` 真在跑；`grep "Do not add an MCP layer" AGENTS.md` 零命中；`docs/en` 与 `docs/zh` 一一对应。
