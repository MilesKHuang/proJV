# 05 - 日常使用

> [English](../../en/user-guide/05-daily.md) | **中文**

> 读完这页你能：管理会话、用 TODO 面板、换主题。

## 会话

每段对话都是 `projv_files/sessions/` 里的一个 SQLite 库，边聊边写。

| 操作 | 方式 |
|------|------|
| 新对话 | `F3` |
| 打开历史 | `F5`（选择器） |
| 清空当前 | `/clear` |
| 压缩长对话 | `/compress` —— 把早期消息摘要掉，保留近期 |

崩溃也不丢：写入按页 checkpoint，写中断时会用 `recovery.db` 恢复。

## AGENTS.md

把 `AGENTS.md` 放在**工作区根目录**，proJV 会在每个新会话里读进它，包在
`[AGENTS.MD] ... [END AGENTS.MD]` 之间。这里放那些从代码里看不出来的东西：
构建与测试命令、项目规矩、绝对不许动的地方。

- 只读根目录这一份：不扫子目录，也不回退读 `CLAUDE.md`。
- 改完要**下一个会话**（`F3`）或 `/clear` 之后才生效，因为这块内容在会话开始时
  就写进了会话。
- 内容上限 16 KiB，超了会截断并在末尾说明。
- 技能读不到它：角色提示词本来就是每次强制载入的规则，而且十个并发 worker
  每人再来一份，成本会翻倍。

## TODO 面板

```text
update_todo(action="init" | "update" | "done", tasks="1. ...", brief="...")
```

面板停靠在右侧（`F9` 开关）。任务完成时它会记一条一句话小结，所以这个面板同时
也是「agent 到底做了什么」的日志。长条目自动折行，不会被截断。

## 主题

- `F6` 切换主题。
- `F7` 打开可视化编辑器：每个颜色都能改、实时预览、JSON 导入导出。
- 2 个内置（`Obsidian`、`Light`）＋ `projv_files/theme/` 里 6 个
  （`artism_warm`、`cyber_punk`、`forest`、`vibrant_focus`、`colorblind_safe`、
  `monochrome_dark`）。
- 选择会写进 `config.toml`，下次启动自动恢复。

## 状态栏

显示模型、角色、token 数、费用估算（按 `config.toml` 的 `[model_prices]`）和上下文
压力。压力高时会用 `compactor` 提示词做压缩。

## diff 气泡

`write_file`、`edit_file`、`md_file`、`format_file` 会显示自己改了什么：工具调用
下面出现一个 `Diff` 块。聊天栏宽度够 100 格时是两栏并排——左边旧行号和旧内容，
右边新行号和新内容，同一处改动左右同排；终端窄了就退回单栏 unified diff
（`+` 是新增，`-` 是删除，`@` 是 hunk 头）。

- 每块最多 40 行，完整补丁留在会话库里。
- 上下各保留 2 行未改动内容，一屏能放下更多 hunk。
- `format_file` 对着已经格式化好的文件再跑一次，不会出现这个块。
- `F9` 的 TODO 面板占 36 格；开着它聊天栏可能跌破并排阈值，diff 就退回单栏。
- diff 只用于显示：随工具那条消息一起落库，所以 `F5` 回放还能看到；但它永远不发
  给模型，不消耗 token。

## 看懂屏幕

| 元素 | 含义 |
|------|------|
| 转圈 + "Thinking..." | 请求进行中 |
| 折叠的一行思考 | 按 `F8` 展开完整思考链 |
| `[角色]` 气泡 | 多 Agent 技能的输出 |
| `── Diff ──` 块 | 上一次写文件改了什么 |
| 右侧 agent 栏 | 每个角色的状态、消息数、当前是否在忙 |

---

下一篇 -> [06 技能](06-skills.md) | 想改它 -> [存储与配置](../dev-guide/module-map.md)
