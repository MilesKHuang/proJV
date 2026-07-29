# proJV 跨平台（Linux + Windows）规划

> 2026-07-27 | 设计阶段

---

## 1. 目标

| # | 需求 |
|---|------|
| 1 | 现有 Windows 版本从 D3D11 + WinHTTP 迁移到 OpenGL + GLFW + libcurl，完成后再新增 Linux 目标 |
| 2 | GUI 层统一为 OpenGL 3.3 + GLFW，双平台共用同一套渲染代码 |
| 3 | 网络层统一为 libcurl，双平台共用同一套 HTTP 代码 |
| 4 | 进程管理与系统工具保留平台差异，通过接口抽象隔离 |
| 5 | 分步骤可编译、可测试，每步独立验证 |

---

## 2. 架构总览

### 2.1 分层

```
┌──────────────────────────────────────────┐
│  UI Layer (app / render_chat / ...)      │  ← 平台无关
├──────────────────────────────────────────┤
│  GUI Backend (OpenGL 3.3 + GLFW)         │  ← 统一实现，双平台共用
├──────────────────────────────────────────┤
│  Process Abstraction (IProcessRunner)    │  ← 接口统一，实现分平台
├──────────────────────────────────────────┤
│  System Abstraction (ISystemUtil)        │  ← 接口统一，实现分平台
└──────────────────────────────────────────┘
```

上层业务逻辑完全平台无关。GUI 层双平台共用同一份 `.cpp`；网络层（libcurl）直接内聚在 `web_tools.cpp` 中，无需单独抽象；进程和系统工具需要 Windows / Linux 各一套实现。

### 2.2 抽象接口清单

| 接口类 | 实现方式 | 职责 |
|--------|---------|------|
| GUI 后端 | **统一实现**，无需接口 | 窗口创建、OpenGL 上下文、ImGui 挂载、渲染循环 |
| `IProcessRunner` | Windows / Linux 各一实现 | 子进程创建、管道通信、等待与终止 |
| `ISystemUtil` | Windows / Linux 各一实现 | 可执行路径、配置目录、字体查找、崩溃处理、路径分隔符 |

### 2.3 文件映射

| 文件 | 说明 |
|------|------|
| `src/gui_backend.cpp` / `.h` | GUI 后端统一实现（OpenGL + GLFW） |
| `src/platform/iprocess_runner.h` | 进程抽象接口 |
| `src/platform/process_runner_win.cpp` | Windows 进程实现 |
| `src/platform/process_runner_linux.cpp` | Linux 进程实现 |
| `src/platform/isystem_util.h` | 系统工具抽象接口 |
| `src/platform/system_util_win.cpp` | Windows 系统工具实现 |
| `src/platform/system_util_linux.cpp` | Linux 系统工具实现 |

---

## 3. 分步实施计划

共 6 个步骤，每步产出可编译运行的二进制。

| 步骤 | 内容 | 产物 | 依赖 |
|------|------|------|------|
| S1 | Windows 端 GUI 从 D3D11 迁移到 OpenGL + GLFW | Windows 可编译运行，行为不变 | 无 |
| S2 | CMake 跨平台构建系统（双平台工程骨架） | CMake 可生成 Windows + Linux 工程 | S1 |
| S3 | Windows 端网络层从 WinHTTP 迁移到 libcurl | Windows 可编译运行，web_search / fetch_url 可用 | S1 |
| S4 | 进程抽象接口 + Linux 进程实现 | Linux 可执行 shell 命令，Windows 通过接口调用行为不变 | S2 |
| S5 | 系统工具抽象接口 + Linux 系统工具实现 | Linux 可获取路径、加载字体、安装崩溃处理 | S2 |
| S6 | 集成完善 + 全功能验证 | Linux 完整可运行，功能对齐 Windows | S3-S5 |

---

## 4. 步骤 S1：Windows GUI 迁移到 OpenGL + GLFW

### 4.1 目标

将 `main.cpp` 中 D3D11 + Win32 窗口代码替换为 OpenGL 3.3 + GLFW，保持 ImGui 渲染效果和交互行为不变。此步仅在 Windows 上验证，不涉及 Linux。

### 4.2 变更内容

**移除**：
- `main.cpp` 中全部 D3D11 设备创建/清理代码（`createDeviceD3D`、`cleanupDeviceD3D`、`createRenderTarget`）
- `main.cpp` 中 `WndProc` 窗口过程、`RegisterClassExW`、`CreateWindowW`
- `main.cpp` 中 `WinMain` 入口函数及消息循环（`PeekMessage` / `DispatchMessage`）
- `#pragma comment(lib, "d3d11.lib")` / `"dxgi.lib"`
- `external/imgui/backends/imgui_impl_win32.cpp` / `imgui_impl_dx11.cpp`（不再编译）

**新增 `src/gui_backend.cpp` / `.h`**：
- `GuiBackend` 类，封装 GLFW 窗口 + OpenGL 上下文 + ImGui 初始化
- `Init(width, height, title)`：`glfwInit` → 设置 OpenGL 3.3 Core Profile → `glfwCreateWindow` → `glfwMakeContextCurrent` → `glfwSwapInterval(1)` → `ImGui::CreateContext` → `ImGui_ImplGlfw_InitForOpenGL` → `ImGui_ImplOpenGL3_Init`
- `NewFrame()`：`glfwPollEvents` → ImGui GLFW/OpenGL3 NewFrame → `ImGui::NewFrame`；检查 `glfwWindowShouldClose`
- `Render()`：`ImGui::Render` → `glClear` → `ImGui_ImplOpenGL3_RenderDrawData` → `glfwSwapBuffers`
- `Shutdown()`：销毁 ImGui 后端 → `ImGui::DestroyContext` → `glfwDestroyWindow` → `glfwTerminate`
- `ShouldClose()`：返回 `glfwWindowShouldClose` 结果
- `GetNativeWindow()`：返回 `GLFWwindow*`

**替换 `main.cpp`**：
- `WinMain` → 标准 `main(int argc, char** argv)`
- 创建 `GuiBackend` 实例 → `Init()` → 主循环 `while (!backend.ShouldClose()) { backend.NewFrame(); g_app.Render(); backend.Render(); }` → `Shutdown()`
- 字体加载逻辑迁移到 `GuiBackend::Init()` 中，Windows 下仍尝试 `assets/msyh.ttc` → `C:\Windows\Fonts\msyh.ttc` → fallback
- 崩溃处理（SEH minidump）暂留在 `main.cpp`，后续 S5 迁移到 `SystemUtilWin`

**引入 GLFW 源码**：
- 从 GLFW 官方 GitHub 拉取稳定 release 源码，放入 `external/glfw/`
- CMake 通过 `add_subdirectory(external/glfw)` 编译，双平台均静态链接，无外部 DLL 依赖

**引入 ImGui 新后端**：
- 从 ImGui 官方获取 `imgui_impl_glfw.cpp` / `imgui_impl_glfw.h` / `imgui_impl_opengl3.cpp` / `imgui_impl_opengl3.h`，放入 `external/imgui/backends/`

### 4.3 验证方法

- 编译通过，启动 proJV.exe
- 窗口正常显示，标题栏、缩放、关闭按钮行为正常
- ImGui 渲染正常：设置页面、聊天界面、主题切换
- 帧率与 D3D11 版本一致（VSync 开启时 60fps）
- 回归测试：角色切换、发送消息、执行 shell 工具

---

## 5. 步骤 S2：CMake 跨平台构建系统

### 5.1 目标

一套 CMakeLists.txt 同时支持 Windows（MSVC）和 Linux（GCC/Clang），通过条件分支选择源文件和依赖。

### 5.2 依赖清单

| 依赖 | Windows | Linux | 用途 |
|------|---------|-------|------|
| OpenGL | 系统自带（opengl32.lib） | 系统自带（libGL） | GUI 渲染 |
| GLFW | 源码编译（external/glfw/） | 源码编译（external/glfw/） | 窗口管理 |
| libcurl | 源码编译（external/curl-8.21.0/） | apt: `libcurl4-openssl-dev` | HTTP 请求 |
| SQLiteCpp | 源码编译（external/） | 源码编译（external/） | 数据库 |
| ImGui | 源码编译（external/） | 源码编译（external/） | UI 框架 |
| toml.hpp | 头文件（external/） | 头文件（external/） | 配置解析 |
| loguru | 源码编译（external/） | 源码编译（external/） | 日志 |
| json.hpp | 头文件（external/） | 头文件（external/） | JSON 解析 |

### 5.3 CMake 结构

**公共部分**（平台无关）：
- 项目声明、C++20 标准
- `external/` 子目录：SQLiteCpp 源码编译；GLFW 源码编译（`add_subdirectory(external/glfw)`）；curl 仅 Windows 走源码编译，Linux 用系统包
- `COMMON_SOURCES`：所有业务逻辑 `.cpp`，含 `gui_backend.cpp`，不含 `platform/` 下的平台实现

**Windows 分支**（`if(WIN32)`）：
- 链接 `opengl32`、Win32 系统库（`comctl32`、`comdlg32`、`shell32`、`ole32` 等）
- MSVC 编译选项（UTF-8 源文件、抑制宏重定义警告）
- 源文件加入 `platform/process_runner_win.cpp`、`platform/system_util_win.cpp`
- 构建后复制 `assets/` 和 `projv_files/` 到输出目录

**Linux 分支**（`if(UNIX)`）：
- `find_package` 查找 OpenGL、CURL 系统库
- 链接 `pthread`、`dl`、X11 相关库（`X11`、`Xrandr`、`Xi`、`Xcursor`、`Xinerama`）
- 源文件加入 `platform/process_runner_linux.cpp`、`platform/system_util_linux.cpp`
- 构建后复制 `assets/` 和 `projv_files/` 到输出目录

**GLFW 说明**：双平台均通过 `external/glfw/` 源码编译并静态链接，版本由仓库锁定，消除 apt / vcpkg 版本差异。OpenGL 本身是系统 API，`opengl32.dll`（Windows）和 `libGL.so`（Linux）由显卡驱动提供，不需要也无法放入 external/；GLFW 在初始化时创建 OpenGL 3.3 Core Profile 上下文，若驱动不支持会直接报错。

### 5.4 验证方法

- Windows：编译通过，产物可运行，GUI 正常
- Linux：CMake 配置阶段不报错（GLFW 从 external/ 编译，无需系统包）；编译阶段仅有链接错误（S4-S5 实现尚未编写）

---

## 6. 步骤 S3：Windows 网络层从 WinHTTP 迁移到 libcurl

### 6.1 目标

`web_tools.cpp` 内的 HTTP 请求从 WinHTTP API 替换为 libcurl easy API。不新建独立文件，直接在 `web_tools.cpp` 内部完成替换，双平台共用同一份代码。

### 6.2 变更内容

**移除 `web_tools.cpp` 中**：
- `#include <windows.h>` / `#include <winhttp.h>` / `#pragma comment(lib, "winhttp.lib")`
- `utf8_to_wide()` / `wide_to_utf8()` 辅助函数
- 现有 `httpGet()` 函数（WinHTTP 实现）

**新增 `web_tools.cpp` 中**：
- `#include <curl/curl.h>` 替代 WinHTTP 头文件
- 重写 `httpGet()` 函数，改为 libcurl 实现：初始化 curl 句柄 → 设置 URL → 设置写回调 → 设置超时（连接 10s，整体 30s）→ 启用重定向 → 设置 User-Agent → `curl_easy_perform` 同步阻塞 → 读取状态码 → 清理句柄 → 返回响应体字符串
- 写回调函数将响应数据追加到 `std::string`

**CMake 调整**：
- Windows 下移除 `winhttp.lib` 链接
- libcurl 在 Windows 和 Linux 上均已存在（Windows 走 `external/curl-8.21.0` 源码编译，Linux 走系统包），无需额外引入

### 6.3 验证方法

- Windows 编译通过
- 测试 `web_search "test query"` 工具，验证返回 DuckDuckGo 搜索结果
- 测试 `fetch_url "https://example.com"` 工具，验证返回页面内容
- 验证超时行为：请求一个不可达的地址，30s 内返回错误

---

## 7. 步骤 S4：进程抽象接口 + Linux 进程实现

### 7.1 目标

定义 `IProcessRunner` 纯虚接口，Windows 实现为现有 `shell_tool.cpp` 中 `execCommand()` 的薄封装，Linux 实现为 POSIX `fork/exec` 方案。`shell_tool.cpp` 和 `python_tool_manager.cpp` 通过接口调用，不再直接依赖平台 API。

### 7.2 接口定义

**`IProcessRunner`**（`src/platform/iprocess_runner.h`）：

- **`ProcessConfig`**：命令字符串、工作目录路径、stdin 内容、超时时间（0 表示无超时）、是否继承环境变量、额外环境变量键值对
- **`ProcessResult`**：stdout 字符串、stderr 字符串、退出码、是否超时、是否被取消
- **`Run(cfg)`**：同步执行子进程，阻塞直到进程退出、超时或外部取消。返回 `ProcessResult`
- **`Cancel()`**：终止当前运行中的子进程。Windows 通过 `TerminateJobObject`，Linux 通过 `SIGTERM` → `SIGKILL` 两段式终止
- **`IsRunning()`**：查询是否有子进程正在执行

### 7.3 Windows 实现

`ProcessRunnerWin` 封装 `shell_tool.cpp` 中现有 `execCommand()` 的 Windows 分支逻辑：
- `CreateProcess`（`CREATE_SUSPENDED`）→ `AssignProcessToJobObject`（`KILL_ON_JOB_CLOSE`）→ `ResumeThread`
- 管道通信（`CreatePipe` + `ReadFile` / `WriteFile`）
- 100ms 轮询等待循环，支持 cancel 检测和 30s 空闲超时
- 独立 reader 线程异步读取 stdout
- 对接 `ToolRegistry` 的 `cancelAll()` 机制

### 7.4 Linux 实现

`ProcessRunnerLinux` 使用 POSIX 接口：
- 管道创建：`pipe()` 创建 3 对文件描述符（stdin、stdout、stderr）
- 子进程：`fork()` → `dup2()` 重定向标准流 → 设置环境变量（阻断 Git 交互式提示）→ `chdir(workspacePath)` → `execvp("/bin/sh", {"sh", "-c", command})`
- 父进程等待循环：关闭管道子端 → 写入 stdin 内容 → 启动 reader 线程 → 100ms 轮询 `waitpid(WNOHANG)` → 检查 cancel 原子标志 → 检查 30s 空闲超时
- Cancel：`Cancel()` 设置原子标志 → 主循环检测后 `kill(pid, SIGTERM)` → 2s 未响应则 `kill(pid, SIGKILL)`
- 进程组管理：`setpgid(0, 0)` 创建新进程组，Cancel 时 `killpg()` 终止整棵进程树

### 7.5 调用方改造

**`shell_tool.cpp`**：
- `execCommand()` 函数移除 `#ifdef _WIN32` / `#else` 分支
- 改为接收 `IProcessRunner*` 参数，调用 `runner->Run(config)` 执行命令
- `registerShellTool()` 中的 lambda 捕获改为通过 `IProcessRunner` 接口调用

**`python_tool_manager.cpp`**：
- `executePyTool()` 中的 `CreateProcess` / `CreatePipe` 等 Win32 API 全部移除
- 改为构造 `ProcessConfig`（含 Python 解释器路径、脚本路径、参数、环境变量），通过 `IProcessRunner::Run()` 执行
- Python 检测逻辑：Windows 通过 `IProcessRunner` 执行 `where python`，Linux 执行 `which python3`

### 7.6 验证方法

**Windows**：
- 编译通过，`exec_shell` 工具行为与当前一致
- Cancel 中断正常（`sleep 30` → Cancel → 100ms 内终止）
- Python 工具（如 `diagram_tool`）正常执行

**Linux**：
- 编译通过
- 测试 `exec_shell`：`ls -la`、`echo hello`、`git status`
- 测试 Python 工具
- 测试 Cancel 和空闲超时

---

## 8. 步骤 S5：系统工具抽象接口 + Linux 实现

### 8.1 目标

定义 `ISystemUtil` 纯虚接口，Windows 实现封装现有 `config.cpp` 和 `main.cpp` 中的平台相关代码，Linux 实现提供对应的 POSIX 版本。

### 8.2 接口定义

**`ISystemUtil`**（`src/platform/isystem_util.h`）：

- **`GetExeDir()`**：返回可执行文件所在目录的绝对路径
- **`GetUserConfigDir()`**：返回配置目录，双平台统一为 `{exeDir}/projv_files/`，用户可直接查看和修改
- **`GetSystemFontPath(fontName)`**：按名称查找系统字体文件的绝对路径，找不到返回空字符串
- **`InstallCrashHandler()`**：安装全局崩溃处理器，崩溃时写日志到 `{exeDir}/proJV_crash.log`
- **`GetPathSeparator()`**：返回平台路径分隔符（`"\\"` 或 `"/"`）
- **`OpenUrl(url)`**：使用系统默认浏览器打开 URL

### 8.3 Windows 实现

`SystemUtilWin`：
- `GetExeDir()`：封装 `GetModuleFileNameA` + `parent_path()`
- `GetUserConfigDir()`：返回 `{exeDir}/projv_files/`（与 Linux 一致）
- `GetSystemFontPath()`：依次搜索 `assets/msyh.ttc` → `C:\Windows\Fonts\msyh.ttc`，返回第一个存在的路径
- `InstallCrashHandler()`：封装 `SetUnhandledExceptionFilter` + `MiniDumpWriteDump` + Event Log 写入（从 `main.cpp` 迁移）
- `OpenUrl()`：封装 `ShellExecuteA`

### 8.4 Linux 实现

`SystemUtilLinux`：
- `GetExeDir()`：`readlink("/proc/self/exe")` → `parent_path()`
- `GetUserConfigDir()`：返回 `{exeDir}/projv_files/`（与 Windows 一致，用户可直接查看和修改）
- `GetSystemFontPath()`：依次搜索 `/usr/share/fonts/`、`~/.local/share/fonts/` 下的 CJK 字体（Noto Sans CJK、WenQuanYi 等），以及 `assets/` 目录下的字体文件，返回第一个存在的路径；全部未找到则返回空字符串，由 GUI 后端 fallback 到 ImGui 内置字体
- `InstallCrashHandler()`：`sigaction(SIGSEGV, ...)` + `sigaction(SIGABRT, ...)`，handler 中通过 `backtrace()` + `backtrace_symbols()` 获取调用栈，写入 `{exeDir}/proJV_crash.log`
- `OpenUrl()`：`fork()` + `execlp("xdg-open", url, NULL)`

### 8.5 调用方改造

**`config.cpp`**：
- `getExeDir()`、`getProjvDir()` 等函数改为调用 `ISystemUtil` 单例的对应方法
- `GetModuleFileNameA` 调用移除

**`main.cpp`**：
- 崩溃处理代码（SEH minidump）迁移到 `SystemUtilWin::InstallCrashHandler()`
- `main()` 启动时调用 `ISystemUtil::InstallCrashHandler()`
- `MessageBoxA` 错误弹窗改为通过 `ISystemUtil` 的日志/通知机制（Linux 无等价物）

**`render_chat.cpp`**：
- `ShellExecuteA` 打开链接改为调用 `ISystemUtil::OpenUrl()`

**`app.cpp` / `theme_popup.cpp` / `render_settings.cpp`**：
- 移除 `#include <windows.h>` / `#include <commdlg.h>`
- 文件对话框（`GetOpenFileNameA`）用 `#ifdef _WIN32` 宏包裹：Windows 保留原有实现，Linux 用简易终端路径输入或 `zenity` 命令行替代

**字体加载**：
- `GuiBackend::Init()` 中字体路径获取改为调用 `ISystemUtil::GetSystemFontPath()`，由各平台实现返回最佳 CJK 字体路径

**`deepseek.cpp`**：
- `Sleep()` 替换为 `std::this_thread::sleep_for()`，双平台通用

### 8.6 验证方法

**Windows**：
- 编译通过，所有功能正常
- 崩溃处理：主动触发空指针解引用，验证 `proJV_crash.log` 和 `.dmp` 文件生成

**Linux**：
- 编译通过
- 验证 `GetExeDir()` 返回正确的可执行文件路径
- 验证配置文件读写：`projv_files/config.toml` 正常加载
- 验证 CJK 字体加载和渲染
- 崩溃处理：`kill -SIGSEGV $(pidof proJV)` 后验证崩溃日志生成

---

## 9. 步骤 S6：集成完善与全功能验证

### 9.1 目标

完成所有模块的集成，处理路径分隔符、字体 fallback、跨平台细节差异，确保 Linux 版本功能完整对齐 Windows。

### 9.2 路径适配

- 项目中所有 `"\\"` 硬编码路径分隔符替换为 `fs::path` 的 `/` 操作符，或调用 `ISystemUtil::GetPathSeparator()`
- `python_tool_manager.cpp` 中 `toolDir + "\\main.py"` 改为 `fs::path(toolDir) / "main.py"`
- 配置文件路径从硬编码拼接改为 `ISystemUtil::GetUserConfigDir() + "/config.toml"`

### 9.3 字体策略

- 优先使用 `assets/` 目录下自带的 CJK 字体文件
- 若不存在，调用 `ISystemUtil::GetSystemFontPath()` 查找系统字体
- 若仍找不到，Fallback 到 ImGui 内置字体（无 CJK 支持，仅显示 ASCII）
- 文档中建议 Linux 用户手动放置字体文件到 `assets/` 目录

### 9.4 全功能验证矩阵

| 验证项 | Windows | Linux |
|--------|:-------:|:-----:|
| 启动与窗口渲染 | ✅ | ✅ |
| 主题切换 | ✅ | ✅ |
| 设置页面（API Key、模型、路径） | ✅ | ✅ |
| 配置读写 | ✅ | ✅ |
| 聊天消息发送与流式响应 | ✅ | ✅ |
| 角色切换（coder / designer / analyzer） | ✅ | ✅ |
| `read_file` 工具 | ✅ | ✅ |
| `grep_files` 工具 | ✅ | ✅ |
| `exec_shell` 工具 | ✅ | ✅ |
| `web_search` 工具 | ✅ | ✅ |
| `fetch_url` 工具 | ✅ | ✅ |
| `md_file` 工具 | ✅ | ✅ |
| `edit_file` 工具 | ✅ | ✅ |
| `diagram_tool` 工具 | ✅ | ✅ |
| Agent 状态机（Streaming → ExecutingTools → Idle） | ✅ | ✅ |
| Cancel 中断（LLM 流 + 工具子进程） | ✅ | ✅ |
| 审批流程（破坏性命令弹窗确认） | ✅ | ✅ |
| 崩溃处理 | ✅ | ✅ |

---

## 10. 改动清单

| # | 文件/目录 | 操作 | 说明 |
|---|----------|------|------|
| 1 | `src/gui_backend.cpp` / `.h` | 新建 | GUI 后端统一实现（OpenGL + GLFW），双平台共用 |
| 2 | `src/platform/iprocess_runner.h` | 新建 | 进程抽象接口 |
| 3 | `src/platform/process_runner_win.cpp` | 新建 | Windows 进程实现（封装现有 `execCommand`） |
| 4 | `src/platform/process_runner_linux.cpp` | 新建 | Linux 进程实现（POSIX fork/exec） |
| 5 | `src/platform/isystem_util.h` | 新建 | 系统工具抽象接口 |
| 6 | `src/platform/system_util_win.cpp` | 新建 | Windows 系统工具实现 |
| 7 | `src/platform/system_util_linux.cpp` | 新建 | Linux 系统工具实现 |
| 8 | `src/main.cpp` | 重构 | 移除 D3D11 / Win32 窗口代码，改为调用 `GuiBackend`；`WinMain` → `main`；崩溃处理迁移到 `SystemUtilWin` |
| 9 | `src/tools/web_tools.cpp` | 重构 | 移除 WinHTTP 代码，直接内嵌 libcurl 实现 HTTP GET |
| 10 | `src/tools/shell_tool.cpp` | 重构 | 移除 `#ifdef` 分支，改为通过 `IProcessRunner` 接口调用 |
| 11 | `src/tools/python_tool_manager.cpp` | 重构 | 移除 Win32 API（`CreateProcess` / `_popen`），改为通过 `IProcessRunner` 接口调用 |
| 12 | `src/core/config.cpp` | 修改 | `GetModuleFileNameA` → `ISystemUtil::GetExeDir()` |
| 13 | `src/ui/render_chat.cpp` | 修改 | `ShellExecuteA` → `ISystemUtil::OpenUrl()` |
| 14 | `src/ui/app.cpp` | 修改 | 移除 `#include <windows.h>` / `<commdlg.h>`；文件对话框用 `#ifdef _WIN32` 宏区分平台 |
| 15 | `src/ui/theme_popup.cpp` | 修改 | 移除 `#include <windows.h>` / `<commdlg.h>`；文件对话框用 `#ifdef _WIN32` 宏区分平台 |
| 16 | `src/ui/render_settings.cpp` | 修改 | 移除 `#include <windows.h>` / `<commdlg.h>` |
| 17 | `src/tools/registry.cpp` | 修改 | 移除无意义的 `#include <windows.h>`（未使用任何 Windows API） |
| 18 | `src/client/deepseek.cpp` | 修改 | `Sleep()` → `std::this_thread::sleep_for()`，双平台通用 |
| 19 | `CMakeLists.txt` | 修改 | 移除 D3D11 / DXGI / WinHTTP 链接；新增 OpenGL / GLFW；条件编译 `platform/` 实现文件 |
| 20 | `external/imgui/backends/` | 新增 | 加入 `imgui_impl_glfw.*` / `imgui_impl_opengl3.*` |
| 21 | `external/imgui/backends/` | 移除 | 不再编译 `imgui_impl_win32.cpp` / `imgui_impl_dx11.cpp` |

---

## 11. 风险 & 缓解

| 风险 | 影响 | 缓解 |
|------|------|------|
| **OpenGL 驱动兼容性**：部分老旧或虚拟化环境 OpenGL 3.3 不可用 | 程序无法启动 | 启动时检测 OpenGL 版本，不满足时弹出对话框提示；文档注明最低要求 |
| **渲染效果差异**：D3D11 与 OpenGL 的 shader / 色彩空间输出可能微调 | 主题外观细微变化 | 在 S1 完成后对比截图，必要时调整 `ImGui::Style` 参数 |
| **libcurl TLS 差异**：Windows 用 Schannel，Linux 用 OpenSSL | 某些 HTTPS 站点行为不同 | 统一设置 TLS 选项，Linux 需安装 `ca-certificates` |
| **进程管理差异**：Windows Job Object 的 `KILL_ON_JOB_CLOSE` 无直接 POSIX 等价 | 子进程树可能残留 | Linux 用 `setpgid` + `killpg` 实现等效的进程组终止 |
| **字体缺失**：Linux 无 `msyh.ttc` | CJK 字符显示为方块 | 提供 fallback 链，文档建议用户放置字体到 `assets/` |
| **路径分隔符**：`\\` 硬编码 | Linux 路径解析失败 | 全局使用 `fs::path` 操作符 |
| **Windows 功能回归** | 用户升级后功能异常 | S1 和 S3 完成后立即执行完整回归测试 |

---

## 12. 编译与测试命令参考

### 12.1 Windows

**配置**：使用 CMake 生成 Visual Studio 工程，指定 Release 配置。
**编译**：通过 CMake `--build` 调用 MSVC，产物位于 `build_win/Release/proJV.exe`。
**运行**：直接双击 exe 或从命令行启动。
**清理**：删除 `build_win` 目录。

### 12.2 Linux

**依赖安装**（Ubuntu/Debian）：需安装 `build-essential`、`cmake`、`libcurl4-openssl-dev`、`libgl1-mesa-dev` 及 X11 相关开发包（`libx11-dev`、`libxrandr-dev`、`libxi-dev`、`libxcursor-dev`、`libxinerama-dev`）。GLFW 无需安装，由 `external/glfw/` 源码编译。
**配置**：使用 CMake 生成 Makefile，指定 Release 配置。
**编译**：`make -j$(nproc)` 并行编译。
**运行**：从终端启动 `./build_linux/proJV`。
**清理**：删除 `build_linux` 目录。

### 12.3 分步测试矩阵

| 步骤 | Windows 测试 | Linux 测试 |
|------|:-----------:|:----------:|
| S1 | ✅ 全功能回归（GUI 已切换为 OpenGL） | — |
| S2 | ✅ 编译通过 | ✅ cmake 配置通过 |
| S3 | ✅ web_search / fetch_url 可用 | — |
| S4 | ✅ exec_shell / python_tool 通过接口调用 | ✅ exec_shell / python_tool 可用 |
| S5 | ✅ 崩溃处理、字体加载正常 | ✅ 路径、字体、崩溃处理可用 |
| S6 | ✅ 全功能回归 | ✅ 全功能验证 |

---

## 13. 附录：平台差异速查

| 维度 | Windows | Linux |
|------|---------|-------|
| 入口函数 | `main` | `main` |
| GUI 后端 | OpenGL 3.3 + GLFW | OpenGL 3.3 + GLFW |
| 网络后端 | libcurl (Schannel) | libcurl (OpenSSL) |
| 进程创建 | `CreateProcess` + Job Object | `fork` + `execvp` + `setpgid` |
| 进程终止 | `TerminateJobObject` | `killpg` + `SIGKILL` |
| 管道 | `CreatePipe` + `ReadFile` | `pipe` + `read` |
| 可执行路径 | `GetModuleFileNameA` | `readlink /proc/self/exe` |
| 配置目录 | `{exeDir}/projv_files/` | `{exeDir}/projv_files/` |
| 崩溃处理 | SEH + `MiniDumpWriteDump` | `sigaction` + `backtrace` |
| 字体 | `C:\Windows\Fonts\msyh.ttc` | `/usr/share/fonts/` 搜索 |
| 编译器 | MSVC | GCC / Clang |
| 链接库 | `opengl32 glfw3 curl ...` | `GL glfw curl pthread dl` |

## 14. 代码比对结果（2026-07-27）

> 按照第 10 节 21 项改动清单逐项验证。

### 14.1 已完成（18 项）

| # | 文件 | 说明 |
|---|------|------|
| 1 | `src/gui_backend.cpp/.h` | GUI 后端统一实现，调用 ISystemUtil::GetExeDir/GetSystemFontPath |
| 2 | `src/platform/iprocess_runner.h` | 进程抽象接口 |
| 3 | `src/platform/process_runner_win.cpp` | Windows CreateProcess+JobObject |
| 4 | `src/platform/process_runner_linux.cpp` | Linux fork/exec+killpg |
| 5 | `src/platform/isystem_util.h` | 系统工具接口 |
| 6 | `src/platform/system_util_win.cpp` | Win: GetExeDir, CrashHandler, OpenUrl, GetSystemFontPath |
| 7 | `src/platform/system_util_linux.cpp` | Linux: /proc/self/exe, sigaction+backtrace, xdg-open, font search |
| 8 | `src/main.cpp` | GuiBackend + main(), WinMain/D3D11 全部移除, crash handler 已迁移到 SystemUtilWin |
| 9 | `src/tools/web_tools.cpp` | WinHTTP 全部移除, libcurl + Bing search |
| 10 | `src/tools/shell_tool.cpp` | 已通过 IProcessRunner 接口调用 |
| 12 | `src/core/config.cpp` | GetModuleFileNameA 移除, ISystemUtil::GetExeDir/GetUserConfigDir |
| 16 | `src/ui/render_settings.cpp` | `#include <windows.h>` 已用 `#ifdef _WIN32` 包裹 |
| 17 | `src/tools/registry.cpp` | `#include <windows.h>` 已移除 |
| 18 | `src/client/deepseek.cpp` | 6处 `Sleep()` 已替换为 `std::this_thread::sleep_for()` |
| 19 | `CMakeLists.txt` | 双平台 `if(WIN32)`/`if(UNIX)` 分支, platform/ 文件条件编译 |
| 20 | `external/imgui/backends/` | imgui_impl_glfw.* + imgui_impl_opengl3.* 已加入 |
| 21 | `external/imgui/backends/` | imgui_impl_win32.cpp + imgui_impl_dx11.cpp 已移除 |

### 14.2 未完成（3 项功能 + 3 处 Linux 空壳）

| # | 文件 | 问题 | 影响 |
|---|------|------|------|
| **11** | `src/tools/python_tool_manager.cpp` | `executePyTool()` 仍直接使用 `CreateProcessW` / `_popen`，未通过 IProcessRunner | **Linux diagram_tool 不可用** |
| **13** | `src/ui/render_chat.cpp` | `ShellExecuteA` 已在 S5 移除，但未接入 `SystemUtil::Instance().OpenUrl()`——链接点击功能丢失 | **双平台链接都点不开** |
| **14** | `src/ui/app.cpp::saveDialogToFile/loadDialogFromFile` | `#ifdef _WIN32` 存在，但 `#else` 分支仅 `(void)agent;` | **Linux Open Chat / Save As 不可用** |
| **14** | `src/ui/app.cpp` | `PostQuitMessage(0)` 已替换为 `glfwSetWindowShouldClose`，但未引入 `#include <GLFW/glfw3.h>` | Windows 上编译 OK（通过 gui_backend.h 间接包含），但耦合脆弱 |
| **15** | `src/ui/theme_popup.cpp` | "Load..." 按钮的 `#ifdef _WIN32` 存在，`#else` 分支仅注释 `// S5: Linux file dialog via zenity`，无实现 | **Linux 主题导入不可用** |

### 14.3 已实现但需注意

| 项 | 说明 |
|----|------|
| Crash handler | `system_util_win.cpp` 已实现 `InstallCrashHandler()`（SEH+MiniDump），`main.cpp` 已调用。Linux 侧 `sigaction+backtrace` 也已实现。 |
| Font 加载 | `gui_backend.cpp` 通过 `SystemUtil::Instance().GetSystemFontPath()` 三层 fallback：bundled → system → builtin。 |
| Path separator | `python_tool_manager.cpp` L196 已改为 `fs::path(toolDir) / "main.py"`。 |
| 双平台编译 | Windows MSVC + Linux GCC 11.4 (WSL) 均编译通过，零错误。 |
