# 08 - 参考

> [English](../../en/user-guide/08-reference.md) | **中文**

> 快捷键、`/` 命令、`config.toml` 配置项，都在这一页。

## 快捷键

| 键 | 作用 |
|----|------|
| `F1` | 关于 |
| `F2` | 配置 |
| `F3` | 新对话 |
| `F4` | MCP server（开关 / 重启） |
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
| `/undo` | 撤销上一轮的文件改动；`/undo!` 连你手动改过的也一起丢弃 |
| `/restore N` | 把工作区回滚到第 N 轮开始前；需要 `/restore N confirm` |
| `/workspace` | 打印当前工作区 |
| `/wish <想要的成品>` | 许愿模式：要你自己定下待决问题、一直做到 TODO 清单清空。单独 `/wish` 看进度，`/wish cancel` 停下并保留清单 |
| `/end_task <id>` / `/end_task all` | 停掉后台任务（整棵进程树一起杀） |
| `/approval` | 查看当前审批档位和"永久允许"的命令；`/approval clear` 全部忘掉 |
| `/skill list` | 列出已安装技能 |
| `/<技能> <主题>` | 运行技能，例如 `/monica D:\proj` |
| `@<路径>` | 附加图片（仅 Flash 模型） |

在输入框里打 `/`，上方会列出命令：继续打字实时过滤，`↑`/`↓` 移动，`Enter` 把命令填进输入框（**不发送**），`Esc` 只关清单。打了空格（如 `/monica `）清单自动消失，不会挡住你要打的主题。

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
| `exec_shell` | 执行命令（需要审批，最长 30 分钟）。长输出保留**最后** 400 行（`tail_lines` 10-2000，或 `full_output` 最多 200 KB）。`background: true` 会作为 `bg#N` 转到后台 |
| `web_search` | DeepSeek 搜索 API |
| `fetch_url` | DeepSeek 抓取 API（返回 Markdown） |
| `update_todo` | TODO 面板：`init` / `update` / `done` |
| `wish_decide` | 只在许愿模式下：记录一个替你做的决定 |
| `wish_block` | 只在许愿模式下：放弃并说明缺什么 |
| `task_output` | 读后台任务的输出（与 `exec_shell` 同样的尾部截断） |
| `run_skill` | 在对话中启动一个技能 |
| `view_image` | 看磁盘上的图片；图片在下一轮以 user 消息送达（仅 Flash 模型） |

`diagram_tool` 不是内置的，它是示例 pytool（`projv_files/pytool/diagram_tool/`，
需要 Graphviz）。

## config.toml

| 键 | 含义 |
|----|------|
| `api_key` | DeepSeek Key |
| `model` | `deepseek-v4-flash`（默认）或 `deepseek-v4-pro` |
| `base_url` | 接口地址，默认 `https://api.deepseek.com` |
| `max_tokens` | 回复预算（默认 65536）。超出 [1024, 65536] 的值在加载时被钳制并告警；撞到 `finish_reason=length` 后的重试不会降低它 |
| `temperature` | 默认 0.0（思考模式下官方不生效） |
| `reasoning_effort` | 思考强度：`none` / `low` / `high` / `max`，默认 `max`；`none` 表示关闭思考（官方默认强度为 `high`） |
| `context_window` | 0 = 自动 |
| `theme` | 当前主题名 |
| `osc_status` | `true`（默认）：用 OSC 7501 把 `working` / `blocked` / `done` / `error` 上报给宿主终端；不认识的终端会直接忽略，输出被重定向时不多写一个字节。正常退出留 `clear`；中断退出（Ctrl-C、关窗口）留 `error`，让这次运行在宿主列表里仍然可见。`PROJV_NO_OSC=1` 强制关闭 |
| `cpp_compiler_path` | 可选，编译类任务用 |
| `python_path` | pytool 解释器；留空 = `python` |
| `clang_format_path` | `format_file` 用的二进制；留空 = 走 PATH |
| `[model_prices]` | 模型前缀 -> 每百万 token 的输入/输出价，用于费用估算 |
| `snapshots_max_workspace_gb` | 默认 2。工作区超过这个大小就关闭 `/undo`（门槛的意义是别把一棵巨树每轮复制一遍）；`0` 表示关掉门槛 |
| `snapshots_keep_days` | 默认 7。超过这个天数的快照在启动时清理 |
| `approval_mode` | `strict` / `balanced`（默认）/ `loose`。**strict**：删除、覆盖、重定向、`git commit`/`push` 全部要审批，且**没有 ALWAYS**；**balanced**：破坏性命令要审批，`build > log.txt` 这种功能性重定向直接跑；**loose**：只有格式化和分区会被拒绝。旧键 `auto_approve` 已退役，仍按 `true` -> balanced、`false` -> strict 解读 |
| `approval_grants` | 你点过 ALWAYS 的命令（逐字原文）。由弹窗写入，用 `/approval` 查看、`/approval clear` 清空；重启和 `/clear` 都不会丢 |

`F2` 能改上面除 `[model_prices]` 以外的每一个键：API key、base URL、model、`max_tokens`、temperature、`reasoning_effort`、`context_window`、三个工具链路径、两个快照键、主题、`osc_status`、审批档位，以及已记住的命令列表（带清空按钮）。填的值按加载时同一套范围钳制，面板写不出加载器会拒绝的值。

## 限制

| 限制 | 值 |
|------|-----|
| Shell / pytool 超时 | 30 分钟，一次一个进程 |
| 后台任务超时 | 60 分钟，到点杀掉并标记 `timed out` |
| 后台输出保留 | 每个任务最后 100 KB（临时文件，退出时删除） |
| `write_file` 单片 | 8 KB |
| `read_file` 整文件 | 100 KB（更大请用行区间） |
| `grep_files` 命中 | 默认 100，最多 500 |
| 每条消息图片数 | 8 |

## 常见问题

| 现象 | 原因 |
|------|------|
| `outside the workspace` | 路径不在启动目录（即工作区）下 |
| 技能里工具报 "not allowed" | 该技能的 `tools` 列表没包含它 |
| `cannot dispatch to a driver agent` | 派活只有一层，这是设计如此 |
| `[skill] config: unknown key 'x'` | `config.json` 里写错了键名，值被忽略 |
| `format_file` 说没找到 | 装 `clang-format`，或配 `clang_format_path`。Visual Studio 自带一个：`...\VC\Tools\Llvm\x64\bin\clang-format.exe` |
| 「TLS certificate verification failed」 | proJV 会校验证书，且**没有关闭校验的开关**。若公司代理或网关重签 TLS，把该代理的根证书装进系统信任存储（Windows：受信任的根证书颁发机构；Linux：`/usr/local/share/ca-certificates` + `update-ca-certificates`）。另外检查系统时间：日期不对也是同样的报错。 |
| 执行 shell 命令前弹了窗 | 这条命令会删除或覆盖真实数据（`rm`/`del`、`mv`/`cp` 覆盖已存在的文件、`git clean`/`reset --hard`/`commit`/`push`、内联 `python -c`/`node -e` 里删文件或写文件的调用）。`approval_mode = balanced` 下重定向（`> log.txt`）和只读的一行式脚本（`py -3 -c "import sqlite3; ..."`）不弹。「执行一次」= 现在跑；「总是」= 现在跑，**并把这条原文记进 `config.toml`**，以后不再问；「否」= 整批工具调用作废并告诉模型。`strict` 档没有「总是」。 |
| `blocked by policy: ... cannot be undone` | 格式化或分区类命令（`format`、`diskpart`、`mkfs`、`fdisk`、`parted`、`dd of=/dev/...`）。不弹窗、不可放行：真要做请自己在终端里执行。 |
| 日志在哪？ | `projv_files/logs/proJV-YYYY-MM-DD.log`，一天一个文件，保留 7 天。崩溃会额外在同目录写 `crash-<时间戳>.log` 与 `.dmp`。Release 版只写 WARNING 及以上，Debug 版全量。 |

---

下一篇 -> 回到[目录](README.md) | 想改它 -> [开发指南](../dev-guide/README.md)
