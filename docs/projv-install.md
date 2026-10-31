# proJV 跨平台安装与 shim（--install / cmake --install）

> 从 docs/archive/projv-osc7501-support.md 的附录 D 拆出。与 OSC 7501 无关：两者只是出自同一批设计讨论。
> 状态：设计稿，未实现。

---

## 1. 目标体验

判断标准只有三条：**一条命令装完、不要管理员权限、装完立刻能用。**

Windows：

```
cmake -S . -B build
cmake --build build --config Release --parallel
cmake --build build --config Release --target install
cd D:\ws\sop-test ; projv          # 立刻可用，无需重开终端
```

Linux：

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
cmake --build build --target install
cd ~/proj ; projv                  # 通常立即；若 ~/.local/bin 不在 PATH 需重开一次 shell
```

下载 ZIP/TGZ 的人（不编译）：

```
解压到任意处  →  projv_tui --install  →  projv 可用
```

## 2. 现状与差距

`CMakeLists.txt` 已有：`install(TARGETS proJV_tui RUNTIME DESTINATION .)`、`projv_files/`（排除 config.toml / sessions / prompts）、`skills/`、README/LICENSE、剪掉依赖残留的 `install(CODE)`，以及 CPack（Windows 出 ZIP、Linux 出 TGZ）。**布局是对的**：exe 与 `projv_files/` 同目录，天然便携。

缺三块：

| # | 缺什么 | 后果 |
|---|---|---|
| 1 | 默认 prefix 是 `C:\Program Files\proJV` / `/usr/local` | 需要管理员 / sudo；普通用户装不上 |
| 2 | 装完不在 PATH | 仍然要敲全路径 |
| 3 | 没有"一条命令装好"的入口 | 下载 ZIP 的人得手动搬文件 |

## 3. 目录与 shim（两平台对照）

| | Windows | Linux | macOS（未来） |
|---|---|---|---|
| 安装目录（默认） | `%LOCALAPPDATA%\Programs\projv` | `~/.local/share/projv` | `~/Library/Application Support/projv` |
| 为什么 | 用户级安装的通行做法（VS Code 用户版同款），免管理员 | 免 sudo，也不与发行版包管理冲突 | 同义 |
| 命令形态 | `projv.cmd` + `projv_tui.cmd`（4 行，绝对路径，`exit /b`） | **符号链接** → `<安装目录>/projv_tui`（回退：`sh` 脚本，必须 `exec`） | 同 Linux（见 D.7） |
| 放在哪 | `%LOCALAPPDATA%\Microsoft\WindowsApps`（已在 PATH、当前用户可写，实测） | 第一个"在 PATH 且可写"的目录，通常 `~/.local/bin` | 同 Linux |
| 生效时机 | **立即** | 通常立即；否则追加 PATH 后需重开 shell | 同 Linux |

两个形态差异的原因：

- Windows 建符号链接需要管理员或开发者模式 → 用转发脚本；
- Linux 的 `GetExeDir()` 走 `readlink(/proc/self/exe)`（`system_util_linux.cpp:21-23`），符号链接会解析到真身，`projv_files` 不会找错；且不引入额外进程，信号与退出码直达 TUI。

## 4. 子命令与语义

| 命令 | 谁用 | 做什么 |
|---|---|---|
| `projv_tui --install` | 下载 ZIP/TGZ 的人 | **拷贝整包**到安装目录 + 写 shim +（必要时）追加 PATH |
| `projv_tui --post-install` | `cmake --install` 之后 | 文件已由 CMake 铺好，**只写 shim** |
| `projv_tui --uninstall` | 想收回命令的人 | 只删**带标记**的 shim（`projv-shim v1`），保留数据 |
| `projv_tui --purge` | 彻底清除 | 删安装目录；若当前就在该目录内运行则拒绝执行 |

**拷贝语义（升级安全的关键）**：

| 内容 | 行为 |
|---|---|
| `projv_tui` 本体 | 覆盖（升级） |
| `fonts/ theme/ pytool/ skills/` | **仅缺失时补**，不覆盖用户改过的 |
| `config.toml` / `sessions/` / `logs/` / `prompts/` | **绝不触碰**（用户数据，首次启动自生成） |

自检输出（装完必须打印）：

```
installed : D:\...\Programs\projv
shim      : C:\...\Microsoft\WindowsApps\projv.cmd  (in PATH: yes)
projv     : resolves to D:\...\Programs\projv\proJv_tui.exe
next      : cd <项目目录> && projv
undo      : projv_tui --uninstall
```

## 5. CMake 侧接入（约 30 行）

```cmake
# 1) 默认装到用户目录；用户显式给了 -DCMAKE_INSTALL_PREFIX 就不覆盖
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  if(WIN32)
    set(CMAKE_INSTALL_PREFIX "$ENV{LOCALAPPDATA}/Programs/projv" CACHE PATH "" FORCE)
  elseif(APPLE)
    set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/Library/Application Support/projv" CACHE PATH "" FORCE)
  else()
    set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/.local/share/projv" CACHE PATH "" FORCE)
  endif()
endif()

# 2) 现有的 install(TARGETS ...) / install(DIRECTORY ...) 保持不动

# 3) 最后一步：让程序自己写 shim（只 warning，不阻断打包）
install(CODE [[
  set(_exe "${CMAKE_INSTALL_PREFIX}/proJV_tui${CMAKE_EXECUTABLE_SUFFIX}")
  execute_process(COMMAND "${_exe}" --post-install RESULT_VARIABLE _r)
  if(NOT _r EQUAL 0)
    message(WARNING "proJV 已安装，但未创建 projv 命令；请手动运行: ${_exe} --post-install")
  endif()
]])
```

三个坑：路径含空格必须加引号；Linux 可执行名无 `.exe`（用 `${CMAKE_EXECUTABLE_SUFFIX}`）；打包机/交叉编译时二进制可能跑不起来 → 只 warning。

## 6. 单测与验收

| 类型 | 用例 |
|---|---|
| 纯函数 | 假 PATH + 临时目录 + 权限位 → 目录选择顺序与降级正确 |
| shim 内容 | Windows：含绝对路径与 `exit /b %ERRORLEVEL%`，空格/`%` 转义正确；Linux：符号链接指向真身，脚本回退含 `exec` 且 0755 |
| 拷贝语义 | 二次 `--install` 不覆盖 `config.toml` / `sessions/`；`skills/` 只在缺失时补 |
| 幂等与卸载 | 重复安装无副作用；`--uninstall` 只删带标记的文件，遇同名外来文件拒绝并提示 |
| PATH 追加 | 只追加一次、带标记、先备份原文件 |
| 覆盖开关 | `PROJV_HOME=<dir>` / `PROJV_SHIM_DIR=<dir>` 生效（单测必须用它，避免写系统目录） |

验收：在干净的用户账号下，从零到 `cd <任意项目> && projv` 可用，且 `--uninstall` 能完整回退。

## 7. 已知限制

1. **Linux 首次可能需要重开 shell**：若 `~/.local/bin` 不在当前 PATH，只能追加后重开。这是 PATH 机制本身的限制（npm 也一样），必须在输出里明说，别让用户以为装失败。
2. **Windows 走 WindowsApps 属灰区**：那是 Store 别名的目录，第三方写 shim 常见但非官方；将来若发 MSIX 会撞名，届时应改为 AppExecutionAlias。
3. **macOS**：`_NSGetExecutablePath` 可能返回符号链接本身 → `GetExeDir()` 会指到 `~/.local/bin`。支持 macOS 时必须用带 `realpath` 的脚本，或在 `GetExeDir()` 里先 realpath 一次。
4. **未真机验证**：Linux 侧目前只能靠单测与 CI；"Linux 可用"这句话需要一次真机或 CI 运行才能成立。

## 8. 工作量与已定项

| 项 | 估时 |
|---|---|
| C++：四个子命令 + 拷贝语义 + shim + PATH 追加 | ~300 行 |
| CMake：默认 prefix + 尾部 `--post-install` | ~30 行 |
| 单测（含 `PROJV_HOME` / `PROJV_SHIM_DIR` 覆盖） | ~100 行 |
| 文档：`docs/{en,zh}/user-guide/01-install.md` 各一节 | 0.3 h |
| **合计** | **1–1.5 天** |

**已定**（本轮确认）：默认安装目录用 Windows `%LOCALAPPDATA%\Programs\projv` / Linux `~/.local/share/projv`；`--install` 负责拷贝整包；命令名 `projv` 与 `projv_tui` 都建；Windows 优先 WindowsApps、不可写再退回自建 bin + 改用户 PATH。