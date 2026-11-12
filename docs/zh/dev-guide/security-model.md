# 安全边界

> proJV 能碰什么、碰不到什么。写这一页是因为"文件工具有路径检查"很容易被
> 误读成沙箱 —— 它不是。

## 有边界的部分

- **文件工具**（`read_file`、`write_file`、`edit_file`、`md_file`、
  `format_file`、`file_search`、`grep_files`）都走 `ToolPaths`
  （`src/tools/tool_paths.cpp`）：两个可写根（工作区与 `projv_files/`），
  外加 `projv_files/sessions/`、`projv_files/logs/` 两个只读子根。
  根外路径直接以 `__WORKSPACE_VIOLATION__` 拒绝，不会落到文件系统。
- **会删除或覆盖的 shell 命令。** `exec_shell` 执行前先分类
  （`src/core/delete_guard.cpp`）：
  - `Ask`：删除类（`rm`、`del`、`rmdir`、`Remove-Item`）、覆盖类（`mv`、
    `cp`、重定向到真实文件）、git 破坏类（`git clean`、`reset --hard`、
    `checkout --`、`stash drop`）、内联解释器代码（`python -c`、`node -e`）。
    一条完整命令文本一次弹窗；"本会话允许"只对该条命令生效，不是整个会话。
  - `Deny`：格式化与分区（`format`、`diskpart`、`mkfs.*`、`fdisk`、
    `parted`、`wipefs`、`sgdisk`、`dd of=/dev/*`）。不弹窗，不可放行。
  - 其余命令直接执行。

## 没有边界的部分

- **`exec_shell` 就是一个 shell。** 放行之后它能读写当前用户能读写的任何
  路径、能出网、能启动任意程序。上面的分类器只拦它认得出的命令；一个运行时
  才删除文件的脚本不会被检查。
- **技能与 pytool** 权限相同。`config.json` 里带 `exec_shell` 的技能拥有完整
  shell 能力；pytool 是由主程序拉起的 Python 进程。
- **MCP 服务器** 是外部进程，走 stdio JSON-RPC。它们以当前用户身份运行，在
  `projv_files/mcp.json` 里配置即视为受信任。
- **没有 OS 沙箱。** 没有 seatbelt、没有 bubblewrap、没有容器、没有 seccomp。
  进程运行器用的 Windows Job Object（`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`）
  是生命周期管理，不是隔离。

## 需要真正隔离时

把 proJV 放进容器，只挂载项目目录：

```bash
docker run --rm -it \
  -v "$PWD:/work" -w /work \
  -v "$HOME/.config/projv:/opt/projv" \
  --network none \
  ghcr.io/example/base:latest /work/projv
```

比镜像更重要的两条：不要挂 `~/.ssh`；API key 用环境变量或挂载配置文件传入，
不要烤进镜像。

## 磁盘上存了什么

| 数据 | 位置 | 保护 |
|---|---|---|
| API key | `projv_files/config.toml` | 明文 |
| 会话 | `projv_files/sessions/*.db` | 明文 SQLite，未加密 |
| 日志 | `projv_files/logs/proJV-YYYY-MM-DD.log` | 明文，保留 7 天 |
| 崩溃现场 | `projv_files/logs/crash-*.*` | 文本 + minidump |
| 技能 / pytool / MCP 配置 | `projv_files/` | 明文，用户可改 |

除了你自己发起的 API 请求，没有任何数据离开本机。没有遥测。
