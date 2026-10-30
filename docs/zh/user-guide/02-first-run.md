# 02 - 第一次使用

> [English](../../en/user-guide/02-first-run.md) | **中文**

> 读完这页你能：填 Key、发出第一条消息、审批工具调用、中断一次运行。

## 1. 填 API Key

1. 在 [platform.deepseek.com/api_keys](https://platform.deepseek.com/api_keys) 拿 Key。
2. 启动 `proJV_tui`。没有配置过 Key 时会自动弹出配置窗。
3. 粘贴 Key，按 **Save & Connect**。

Key 保存在 `projv_files/config.toml`（`api_key = "..."`）。这个文件已被 git 忽略，
发布包里也永远不含它。

## 2. 在项目目录里启动

工作区是文件工具**唯一**能碰的目录，它就是**你启动 proJV 时所在的目录**，没有配置项。

```
cd D:\ws\myproject && D:\tools\proJV_tui.exe     # 工作区 = D:\ws\myproject
```

- Linux/macOS 一样：`cd ~/proj && /opt/projv/projv_tui`。
- 双击可执行文件 = 它所在的目录（Windows 默认起始目录）。
- 取不到启动目录时，回退到可执行文件所在目录。
- `/workspace` 查看当前工作区。

所以请在项目目录里启动，而不是在用户目录里启动。

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
| `@路径` 提示 "does not accept image input" | 图片只有 Flash 线支持，`F10` 换成 `deepseek-v4-flash` |
| 每次工具调用都报 "outside the workspace" | 启动目录是不含该文件的上层目录——先 `cd` 进项目目录再启动 |
| 一直转圈没有回答 | 网络或 Key 的问题，看 `projv_files/logs` |

---

下一篇 -> [03 角色](03-roles.md) | 想改它 -> [API 客户端](../dev-guide/module-map.md)
