# proJV 用户指南

> [English](../../en/user-guide/README.md) | **中文**

proJV 是用原生 C++ 写的终端 AI agent（TUI，DeepSeek）。装上就能聊，能读写文件、
执行命令、搜网页，还能跑多 Agent 技能。

## 建议按顺序读

| # | 页面 | 读完之后 |
|---|------|---------|
| 01 | [安装](01-install.md) | 下载即用，或在 Windows / Linux 上自己编译 |
| 02 | [第一次使用](02-first-run.md) | 填好 Key、发出第一条消息、会审批工具、会中断 |
| 03 | [角色](03-roles.md) | 用 Tab 切角色，并写出自己的角色 `.md` |
| 04 | [图片输入](04-images.md) | 用 `@路径` 发图片 |
| 05 | [日常使用](05-daily.md) | 会话、TODO 面板、主题、上下文压缩 |
| 06 | [技能](06-skills.md) | 跑 `bigbang` / `monica`，看懂侧边栏，写自己的技能 |
| 07 | [pytool](07-pytool.md) | 写一个 Python 工具让 AI 调用 |
| 08 | [参考](08-reference.md) | 所有快捷键、`/` 命令、`config.toml` 配置项 |

## 需要什么

- 一个 DeepSeek API Key（目前只支持 DeepSeek）。
- 一个终端。Windows 建议 Windows Terminal；Linux 任意 UTF-8 终端都行，
  SSH 和 tmux 也没问题。

## 它把东西放在哪

全部在可执行文件旁边，`projv_files/` 里：

```
projv_files/
├── config.toml     配置（第一次保存时创建，里面是你的 Key）
├── prompts/        角色提示词（首次运行生成）
├── theme/          主题 JSON
├── pytool/         Python 工具
├── skills/         多 Agent 技能（bigbang、monica）
└── sessions/       SQLite 会话库
```

可执行文件 + `projv_files/` 一起拿走就能跑，这一对就是完整的安装。

---

想深入 -> [开发指南](../dev-guide/README.md)
