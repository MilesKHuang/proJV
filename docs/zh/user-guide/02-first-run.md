# 02 - 第一次使用

> [English](../../en/user-guide/02-first-run.md) | **中文**

> 读完这页你能：填 Key、发出第一条消息、审批工具调用、中断一次运行。

## 1. 填 API Key

1. 在 [platform.deepseek.com/api_keys](https://platform.deepseek.com/api_keys) 拿 Key。
2. 启动 `proJV_tui`。没有配置过 Key 时会自动弹出配置窗。
3. 粘贴 Key，按 **Save & Connect**。

Key 保存在 `projv_files/config.toml`（`api_key = "..."`）。这个文件已被 git 忽略，
发布包里也永远不含它。

## 2. 设置工作区（建议）

工作区是文件工具**唯一**能碰的目录。

- `/workspace` 查看当前工作区。
- `F2` 配置里修改。
- 留空 = 可执行文件所在目录。

请把它设成你要处理的项目目录，而不是整个用户目录。

## 3. 发第一条消息

输入框里写，按 `Enter`。例如：

```
读一下 src/main.cpp，说说它干什么
```

你会依次看到：转圈；流式回答（实时 Markdown 渲染）；工具调用及其结果。

## 4. 审批工具调用

写文件、跑命令这类有副作用的工具会先问你要不要执行。弹窗里有工具名和参数。

- 同意 -> 执行，结果回到模型。
- 拒绝 -> 模型被告知被拒，会换个做法。

## 5. 中断运行

- 运行中按 `Esc`：取消这一轮，正在跑的工具会被打断，程序不退出。
- 空闲时按 `Esc`：退出。
- 任何时候 `Ctrl+C` 退出；会话库会 checkpoint 后关闭。

## 6. 几个面板

| 键 | 面板 |
|----|------|
| `F8` | 思考链（折叠成一行，再按展开） |
| `F9` | TODO 面板 |
| `F2` | 配置 |
| `F5` | 历史会话 |

## 怎么确认成功

- 你的消息在聊天区里，下面跟着回答。
- 状态栏显示模型、角色、token 数、费用估算、上下文压力。

## 不成功怎么办

| 现象 | 原因 |
|------|------|
| 跑技能时提示 "does not support tool calls" | 选了 reasoner 类模型，换成 `deepseek-v4-flash` |
| 每次工具调用都报 "outside the workspace" | 工作区设成了不含该文件的上层目录 |
| 一直转圈没有回答 | 网络或 Key 的问题，看 `projv_files/logs` |

---

下一篇 -> [03 角色](03-roles.md) | 想改它 -> [API 客户端](../dev-guide/module-map.md)
