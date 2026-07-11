# proJV — Windows DeepSeek GUI Agent

<p align="center">
  <strong>简体中文</strong> | <a href="README.md">English</a>
</p>

<p align="center">
  <img src="assets/image.png" alt="proJV screenshot" width="1280"/>
</p>

Project JV, 意思是 **Just Vibing**

基于 **Dear ImGui + DirectX11 + libcurl** 的 DeepSeek AI Agent，C++20 实现。

一个 **vibe coding** 项目——从第一行代码到所有功能，全部由 AI Agent 自主编写，我只负责提需求和点 Approve。

---

## 为什么有它

图像算法工程师，主力语言 C++，也拿 Python 训 NN。喜欢 PC 能打游戏，喜欢轻量的三方库。

这种背景让我对现有的 TUI 工具和"大厂 Agent"提不起兴趣——我不确定那些花花绿绿的插件、精美的终端、莫名其妙的功能对我有啥实质性帮助。VS Code 我真正装的扩展也就那几个。

对我而言，做一个有点 UI 的轻量 Windows Agent，是一件理所当然的事情。

秉持着"能用就好"的原则，业余时间抽空写。这不是一个严谨的项目，更不到产品级别。但幸运的是它已经跨过了"**自我迭代**"这一关——proJV 已经能自己读代码、改代码、编译自己了，而且不会常常崩溃。我也很期待它会变成什么样。

希望你也喜欢它。

---

## 支持功能

| 功能 | 说明 |
|------|------|
| **Agent 自动调用工具** | 读文件、写文件、搜索替换编辑、Shell 执行、文件搜索、Git 操作、网页搜索、网页抓取、TODO 管理 — AI 自主决策调用 |
| **自定义 System Prompt** | 可编辑系统提示词，自由定义 Agent 行为、工具白名单、输出风格 |
| **思考过程展示** | deepseek-reasoner 的 `reasoning_content` 可折叠展示，紫色卡片 |
| **流式输出** | 实时 SSE 流式渲染，Markdown + 代码块高亮 |
| **上下文管理** | Token 估算 + 智能裁剪，到达压力阈值自动压缩 |
| **会话持久化** | SQLite 自动保存全部对话，支持新建 / 保存 / 加载历史会话 |
| **状态栏** | 实时显示模型名、Token 用量（输入+输出）、消息数、工具调用次数、上下文压力百分比 |
| **工具审批** | 破坏性操作（删除文件等）弹窗确认，通过才放行 |
| **TODO 管理** | AI 自主创建、追踪、标记任务，侧边栏实时查看 |
| **主题** | Dark / GitHub Dark 两种配色 |

---

## 快速开始

### 构建

需要 CMake 和 Visual Studio 2019+（或任何支持 C++20 的编译器）。

```bash
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat"

cd proJV
mkdir build && cd build
cmake .. -G Ninja -DCMAKE_CXX_COMPILER=cl -DCMAKE_BUILD_TYPE=Release
cmake --build .
```

项目根目录也附带了 `clean_and_build.bat`，你可以让 AI Agent 帮你改成你需要的脚本。

产物：`build/proJV.exe`（~3MB，依赖 Windows 系统 DLL + VC 运行时）

### 运行

1. 申请 DeepSeek API Key：[platform.deepseek.com](https://platform.deepseek.com/api_keys)
2. 双击 `proJV.exe`，输入 Key → **Save & Connect**
3. 开始对话

API Key 保存在同目录 `config.toml`，下次自动加载。

---

## 架构

```
proJV/
├── CMakeLists.txt
├─┬ assets/
│ └── msyh.ttc              # 微软雅黑字体（CJK 支持）
├── external/                # 静态依赖，无需包管理器
│   ├── imgui/               # Dear ImGui GUI 框架
│   ├── imgui_markdown.h     # Markdown 渲染
│   ├── json.hpp             # JSON 解析（nlohmann）
│   ├── toml.hpp             # TOML 解析（toml++）
│   ├── SQLiteCpp-3.3.3/     # SQLite C++ 封装
│   ├── curl-8.21.0/         # libcurl（HTTP/HTTPS）
│   └── loguru.cpp/hpp       # 日志
├─┬ src/
│ ├── main.cpp               # WinMain + D3D11 + ImGui 主循环
│ ├── debug_log.h            # 日志宏（Release 自动静默）
│ ├─┬ client/                # DeepSeek API 客户端
│ │ └── deepseek.h/.cpp      # libcurl HTTP + SSE 流式请求
│ ├─┬ core/                  # 核心逻辑
│ │ ├── agent.h/.cpp         # Agent 主循环（思考→工具→回复）
│ │ ├── session.h/.cpp       # 对话管理 + Token 估算
│ │ ├── config.h/.cpp        # TOML 配置加载
│ │ ├── storage.h/.cpp       # SQLite 持久化
│ │ ├── storage_queue.h/.cpp # 异步写入队列
│ │ ├── tool_worker.h/.cpp   # 后台工具执行线程
│ │ └── prompts_loader.cpp   # 系统提示词加载
│ ├─┬ ui/                    # 用户界面
│ │ ├── app.h/.cpp           # 主应用 + 窗口管理
│ │ ├── render_chat.cpp      # 聊天气泡 + Markdown 渲染
│ │ └── render_settings.cpp  # 设置面板
│ └─┬ tools/                 # Agent 可调用的工具集
│   ├── registry.h/.cpp      # 工具注册中心
│   ├── shell_tool.cpp       # Shell 命令执行
│   ├── file_tool.cpp        # 文件读写
│   ├── edit_file_tool.cpp   # 搜索替换
│   ├── file_search_tool.cpp # 文件搜索
│   ├── web_tools.cpp        # 网页搜索 + 抓取
│   └── todo_tool.cpp        # TODO 管理
└── build/
    └── proJV.exe            # 编译产物（~3MB）
```

### 数据流

```
用户输入 → Agent（思考/推理）
                ↓
        需要调工具？ → ToolWorker（后台线程执行）
                ↓
        生成回复 → libcurl SSE 流式接收 → UI 气泡渲染
                ↓
         SQLite 异步写入（持久化）
```

---

## 依赖

| 库 | 用途 | 来源 |
|----|------|------|
| [Dear ImGui](https://github.com/ocornut/imgui) | GUI 框架 | `external/imgui/` |
| [imgui_markdown](https://github.com/juliettef/imgui_markdown) | Markdown 渲染 | 单头文件 |
| [SQLiteCpp](https://github.com/SRombauts/SQLiteCpp) | 数据库 | `external/SQLiteCpp-3.3.3/` |
| [libcurl](https://curl.se/) | HTTP/HTTPS | `external/curl-8.21.0/` |
| [loguru](https://github.com/emilk/loguru) | 日志 | `external/loguru.cpp` |
| [nlohmann/json](https://github.com/nlohmann/json) | JSON 解析 | 单头文件 |
| [toml++](https://github.com/marzer/tomlplusplus) | TOML 解析 | 单头文件 |
| DirectX 11 | GPU 渲染 | 系统内置 |

**没有 vcpkg / conan / npm / pip。** 全部依赖都是源码级，`cmake --build` 一次完成。

---

## 致谢

- [DeepSeek](https://deepseek.com) — API 和推理能力
- [Dear ImGui](https://github.com/ocornut/imgui) — 最好的即时模式 GUI
- [SQLiteCpp](https://github.com/SRombauts/SQLiteCpp) — 轻量数据库封装
- [libcurl](https://curl.se/) — 可靠的 HTTP 客户端
- [loguru](https://github.com/emilk/loguru) — 简洁的 C++ 日志
- [imgui_markdown](https://github.com/juliettef/imgui_markdown) — Markdown 渲染
- 所有其他开源依赖的维护者

---

## 请我喝咖啡

如果 proJV 为你节省了时间或带来了乐趣，欢迎请我喝杯咖啡 ☕
*（仅支持中国大陆 / 微信赞赏）*

<p align="left">
  <img src="assets/wechat_donate.jpg" alt="微信赞赏" width="200"/>
</p>
