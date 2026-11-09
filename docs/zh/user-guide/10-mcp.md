# 10 - MCP server

> **中文** | [English](../../en/user-guide/10-mcp.md)

proJV 可以接 [MCP](https://modelcontextprotocol.io) server。一个 MCP server 就是一个
普通的**外部进程**，用 stdin/stdout 讲 JSON-RPC。proJV 把它拉起来、跨回合一直养着，
再把它暴露的每个工具变成模型能调用的工具。

不加载任何代码。你能在终端里跑起来的东西，就能接进来。

## 1. 声明一个 server

清单在可执行文件旁边的 `projv_files/mcp.json`（和 `config.toml` 同一层）。首次启动
会生成一个空文件：

```json
{
  "servers": []
}
```

加一条：

```json
{
  "servers": [
    {
      "name": "time",
      "command": "uvx",
      "args": ["mcp-server-time"],
      "protocol": "stateful",
      "trusted": true,
      "enabled": true,
      "timeout_ms": 120000
    }
  ]
}
```

| 字段 | 必填 | 含义 |
|------|------|------|
| `name` | 是 | 唯一。会成为工具前缀：`mcp__time__get_current_time` |
| `command` | 是 | 可执行文件，按 `PATH` 查找。中间**不经过 shell** |
| `args` | 否 | 参数数组，一个元素 = 一个参数 |
| `env` | 否 | 传给子进程的额外环境变量 |
| `protocol` | 否 | `"stateful"`（默认，legacy `initialize` 握手）或 `"stateless"`（modern：不握手，每个请求自带元数据） |
| `trusted` | 否 | **默认 `false`。没被信任的 server 根本不会被启动** |
| `enabled` | 否 | 默认 `false`。必须显式打开，或用 `F4` |
| `timeout_ms` | 否 | 单个请求超时，默认 `120000` |

写坏的条目（缺 `name`、`protocol` 非法、`name` 重复）只会被跳过并记一行日志，不会
拖垮整张清单。

## 2. 打开它

两种方式，等价：

- 写 `"enabled": true` 然后重启 proJV；或
- 按 `F4`，选中该 server，按 `Enter`。

`F4` 面板还会显示实时状态（`stopped` / `connecting` / `handshaking` / `ready` /
`dead`）和每个 server 贡献了几个工具。`R` 重启选中的 server。开关会写回
`mcp.json` 的 `enabled`，重启后仍然生效。

**回合运行中拒绝开关。** 启停 server 会改工具表，而工具表只允许在回合之间修改。
先把这一回合跑完（或按 `Esc`）。

## 3. 用它的工具

server 一旦 `ready`，它的工具就以 `mcp__<server>__<tool>` 出现，并且**不需要往角色
提示词里加任何东西**模型就能看见——这些名字只在运行期才存在，本来也没法预先写。

通常你不需要点名工具，说清想要的结果就行：

```
东京现在几点？
```

想手工验证接线，不必开 TUI：

```
proJV_tui --mcp-check
proJV_tui --mcp-call time get_current_time timezone=Asia/Tokyo
```

`--mcp-check` 会打印每个 server 的状态与工具清单，只要有"已启用且已信任"的 server
没起来就返回非零。`--mcp-call` 额外执行一次调用。参数是 `key=value`；值能被 JSON
解析就按该类型传，否则按字符串传。

## 4. 安全

`"trusted": false` 是唯一的开关，在**启动进程之前**检查：不受信任的 server 不运行、
不注册工具、也没有任何开销。**没有逐工具审批**——一旦信任并启用，模型可以调用它暴露
的任意工具。

把 `mcp.json` 当成 `config.toml` 对待：它的 `env` 里可能放着 token，它已在
`.gitignore` 里。MCP server 写文件用的是你的用户权限。

## 5. 三个容易踩的点

**进程的工作目录是启动 proJV 的那个目录。** 换工作区必须重启 proJV。如果某个 server
的参数里有 `"."`，它指的就是启动目录。

**`enabled` 是全局的，不是按项目的。** `mcp.json` 在可执行文件旁边，在 A 项目关掉，
B 项目也是关的。

**`uvx` / `npx` 类 server 第一次启动很慢。** 启动器要先下载包——几十 MB，冷缓存下远超
120 秒超时。自己先预热一次：

```
uvx mcp-server-time < nul        （Windows）
uvx mcp-server-time < /dev/null  （Linux）
```

或者把 `timeout_ms` 调大。跳过这一步的表现是：server 卡在 `handshaking`，然后 `dead`。

## 6. 这个版本不做什么

- **只支持 stdio。** 没有 HTTP / SSE 传输。
- **不支持 `tools/list` 分页。** 会分页的 server 会被**整台拒绝**——这是刻意的：
  只注册一半工具会让模型去猜名字。
- **不支持 resources / prompts 方法。** 只有 tools。
- **不支持多轮 input 请求。** server 返回 `input_required` 时会变成一条明确的工具错误。
- **技能用不到 MCP 工具。** 技能的子 agent 是并发的，会共用同一个 server 的 stdin；
  技能 `tools` 里写 `mcp__*` 会在装载时被跳过。

## 7. 排错

| 现象 | 原因 |
|------|------|
| 一直是 `stopped`、工具 0 个 | `trusted` 或 `enabled` 是 false，或名字重复被跳过 |
| `dead`，错误里写 `command not found` | 启动 proJV 的那个环境里 `command` 不在 `PATH` 上 |
| `dead`，错误里提到 `protocol version` | server 拒绝了握手；检查 `protocol`（`stateful` / `stateless`） |
| 卡在 `handshaking` 然后超时 | 启动器还在下载包——见第 5 节 |
| 工具列出来了但模型不调用 | server 是 `ready`，但这一回合在它就绪前就开始了——下一回合就能看见 |
