# 03 - 角色

> [English](../../en/user-guide/03-roles.md) | **中文**

> 读完这页你能：切换角色，并写出自己的角色。

## 角色是什么

角色就是一段系统提示词：`projv_files/prompts/` 下的一个 `.md` 文件，文件名就是
角色名。首次运行只生成一个角色：

| 角色 | 用来做什么 |
|------|-----------|
| `assistant` | 默认，几乎所有活都用它。用 `update_todo` 做计划，写代码和设计文档，持有全套工具 |
| `compactor` | 内部用：上下文压力大时如何压缩早期对话（不在 `Tab` 循环里） |

早期版本会生成三个角色：`coder`、`designer`、`analyzer`。现在合并成一个
`assistant`：计划放在 TODO 列表里，"先计划后执行"不再需要换一个提示词文件——
换文件换来的只是更窄的工具名单，却要养一套阶段切换的习惯。这里没有任何名字被保留：
老安装里还留着这三个文件，它们就是普通角色，文件名就是角色名，`Tab` 里照常出现。
不想让它在循环里，删掉那个文件即可——只有 `compactor.md` 特殊，它是 `/compress` 的
提示词，永远不会当角色。

## 会话中切换

按 `Tab` 循环切换。上下文保留：切的只是系统提示词，不是对话内容。你自己写的角色
也在同一个循环里。

## 写自己的角色

1. 新建 `projv_files/prompts/reviewer.md`。
2. 用普通 Markdown 写指令。要说清输出格式，也要说清「不要做什么」。
3. 用一行写清它能用什么：`tools available: read_file, grep_files`。
4. 按 `Tab`（或重启），`reviewer` 就出现在循环里了。

最小骨架：

```markdown
你是代码审查员。只报告，不改文件。

tools available: read_file, grep_files

## 步骤
1. read_file 目标文件。
2. 以 `文件:行 - 严重度 - 问题` 的形式列出发现。
3. 最后给出最重要的一条修复建议。
```

## 角色管得到的东西

- **工具**。`tools available:` 那一行是权限表，不是提示：agent 只把名单里的工具
  给模型，调用名单外的工具会被直接拒绝。不写这一行等于**放行全部工具**，所以只读
  角色必须写。默认的 `assistant.md` 故意不写：提示词里再抄一份工具名单已经过期过
  一次，现在这份清单由每轮重建的能力地图负责。
- **能碰哪些文件**。所有角色权限相同：workspace 和 `projv_files`（prompts、skills、
  theme、pytool、`mcp.json`）可读可写，`projv_files/sessions`、`projv_files/logs`
  只读，其余一律拒绝。所以角色可以给自己加能力：「加一个 skill」「加这个 MCP
  server」都是普通写文件，不是特殊命令。
- **行为**：先读什么、答案长什么样、不许动什么。

角色提示词是这次会话的第 **P0** 条消息。后面依次是环境块（P1）和项目块（P2），而
能力地图（P3）每轮都会重建 —— 所以加 skill、加 MCP server 都不需要新建角色文件。

## 改完什么时候生效

| 你改的东西 | 生效时机 |
|------------|----------|
| `projv_files/prompts/*.md` | 下次 `F3`（新会话）或 `/clear` |
| `projv_files/skills/<name>/` | 下次调用 `run_skill`，不用重启 |
| `projv_files/mcp.json` | 你下一条消息 |
| `projv_files/pytool/<name>/` | 保存配置（`F2`）后，或重启 |
| `projv_files/theme/*.json` | 重启后（主题菜单在启动时构建一次） |

## 角色管不到的东西

- **角色配色**来自当前主题（`speaker_colors`），不是提示词文件。

---

下一篇 -> [04 图片输入](04-images.md) | 想改它 -> [提示词与生成](../dev-guide/module-map.md)
