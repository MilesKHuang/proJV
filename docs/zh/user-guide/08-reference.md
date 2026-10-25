# 08 - 参考

> [English](../../en/user-guide/08-reference.md) | **中文**

> 快捷键、`/` 命令、`config.toml` 配置项，都在这一页。

## 快捷键

| 键 | 作用 |
|----|------|
| `F1` | 关于 |
| `F2` | 配置 |
| `F3` | 新对话 |
| `F5` | 打开历史会话（选择器） |
| `F6` | 切换主题 |
| `F7` | 主题编辑器 |
| `F8` | 思考链折叠 / 展开 |
| `F9` | TODO 面板 |
| `F10` | 切换模型（从 API 拉取） |
| `Tab` | 切换角色 |
| `↑` / `↓` | 按行滚动聊天区 |
| `PgUp` / `PgDn` | 按页滚动 |
| `Enter` | 发送 |
| `Esc` | 运行中取消 / 空闲时退出 |
| `Ctrl+C` | 退出（会话先 checkpoint 再关闭） |

## 命令

| 命令 | 作用 |
|------|------|
| `/help` | 聊天内帮助 |
| `/clear` | 清空当前会话 |
| `/compress` | 摘要早期消息 |
| `/workspace` | 打印当前工作区 |
| `/skill list` | 列出已安装技能 |
| `/<技能> <主题>` | 运行技能，例如 `/monica D:\proj` |
| `@<路径>` | 附加图片（仅 Flash 模型） |

## 内置工具

| 工具 | 作用 |
|------|------|
| `read_file` | 读文件；大文件用 `start_line`/`end_line`（单次 1000 行 / 100KB） |
| `write_file` | 写小文件（8KB 上限，`mode="append"` 追加） |
| `edit_file` | 精确替换一段文本 —— 改代码的正道 |
| `md_file` | 写 / 改 `.md`（模板、按节替换） |
| `grep_files` | 按正则搜工作区；`max_results` 最大 500 |
| `file_search` | 按文件名找文件 |
| `list_dir` | 目录树，支持 `depth` 与 `exclude` |
| `format_file` | clang-format，或纯空白清理兜底 |
| `exec_shell` | 执行命令（需审批，上限 30 分钟） |
| `web_search` | DeepSeek 搜索 API |
| `fetch_url` | DeepSeek 抓取 API（返回 Markdown） |
| `update_todo` | TODO 面板：`init` / `update` / `done` |
| `run_skill` | 在对话中启动一个技能 |

`diagram_tool` 不是内置的，它是示例 pytool（`projv_files/pytool/diagram_tool/`，
需要 Graphviz）。

## config.toml

| 键 | 含义 |
|----|------|
| `api_key` | DeepSeek Key |
| `model` | `deepseek-v4-flash`（默认）或 `deepseek-v4-pro` |
| `base_url` | 接口地址，默认 `https://api.deepseek.com` |
| `max_tokens` | 单次回复预算（默认 65536） |
| `temperature` | 默认 0.0 |
| `workspace_path` | 文件工具唯一可访问的目录；留空 = 可执行文件所在目录 |
| `context_window` | 0 = 自动 |
| `theme` | 当前主题名 |
| `cpp_compiler_path` | 可选，编译类任务用 |
| `python_path` | pytool 解释器；留空 = `python` |
| `clang_format_path` | `format_file` 用的二进制；留空 = 走 PATH |
| `[model_prices]` | 模型前缀 -> 每百万 token 的输入/输出价，用于费用估算 |

## 限制

| 限制 | 值 |
|------|-----|
| shell / pytool 超时 | 30 分钟，且同一时间一个进程 |
| `write_file` 单片 | 8 KB |
| `read_file` 整文件 | 100 KB（更大请用行区间） |
| `grep_files` 命中 | 默认 100，最多 500 |
| 每条消息图片数 | 8 |

## 常见问题

| 现象 | 原因 |
|------|------|
| `outside the workspace` | 路径不在 `workspace_path` 下 |
| 技能里工具报 "not allowed" | 该技能的 `tools` 列表没包含它 |
| `cannot dispatch to a driver agent` | 派活只有一层，这是设计如此 |
| `[skill] config: unknown key 'x'` | `config.json` 里写错了键名，值被忽略 |
| `format_file` 说没找到 | 装 `clang-format`，或配 `clang_format_path` |

---

下一篇 -> 回到[目录](README.md) | 想改它 -> [开发指南](../dev-guide/README.md)
