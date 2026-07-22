# proJV v0.4.4 Release Notes

> Date: 2026-07-21
> Compare: `v0.4.2` → `v0.4.4`

---

## Highlights

### PyTool System (New)
Python 工具与 C++ 内建工具平权注册，无需修改 `ToolRegistry` 核心接口。

- **`PythonToolManager`**: 启动时自动扫描 `{exeDir}/projv_pytool/`，解析 `tool.json` 元数据，将合规 Python 工具注册到 `ToolRegistry`。Lambda 捕获 `toolDir`/`pythonPath`/`workspacePath`，与 C++ 工具完全平权。
- **`executePyTool()`**: Agent 线程同步子进程——`CreateProcess` + stdin 传参（避免 Windows 命令行 8191 字符限制），stdout 截断 32KB，stderr 错误返回。
- **`CMakeLists.txt` POST_BUILD**: 编译时自动复制 `projv_pytool/` 到 build 输出目录。

### diagram_tool (New PyTool)
Graphviz DOT 语言渲染，`dot.exe` 自动下载。

- **`find_dot()`**: `PATH` → `{tool_dir}/bin/` → 自动从 GitLab 下载便携版 Graphviz，提取 `dot.exe` + DLL 到 `{tool_dir}/bin/`。
- **输出路径**: `output: "proJV/doc/diagram"` → `{workspace}/proJV/doc/diagram.png`，支持子目录。
- **零外部依赖**: 仅需 `pip install graphviz`（Python 绑定），`dot.exe` 首次使用时自动获取。

### Multi-Role Prompt System v2 (New)
从 3 角色扩展到 6 角色，supervisor 为默认。

| 角色 | 文件 | 工具 | 用途 |
|------|------|:---:|------|
| **supervisor** ★ | supervisor.md | 11 | 项目开发助手（全能，默认） |
| coder | coder.md | 7 | 编码实现（去掉 web_search/fetch_url） |
| designer | designer.md | 3 | 架构设计 |
| analyzer | analyzer.md | 5 | 项目分析 + 架构图（diagram_tool） |
| tester | tester.md | 6 | 测试编写 + 编译执行 + 回报 |
| compactor | compactor.md | 0 | 上下文压缩（内部，UI 隐藏） |

- `prompts_loader.cpp`: 新增 `PROMPT_DEFAULT_SUPERVISOR`/`ANALYZER`/`TESTER` 三个内置预设；coder 工具行精简。
- `app.cpp`: 默认角色从 `coder.md` 改为 `supervisor.md`。
- `ensureDefaultPrompts()`: 启动时自动创建缺失的 `.md` 文件。

### threading_model v2.1 (Updated)
- 审查并修正：**`StorageWriteQueue` 不存在**（虚构独立线程），实际 Agent 线程同步写 DB，WAL 双连接保证安全。
- 新增：`PyTool` 同步子进程执行路径、`fetchModelsThread_`（启动临时线程）。
- 3 张 Graphviz DOT 渲染图：线程拓扑、Agent 状态机、线程交汇点。

---

## Architecture Changes

| Area | Before (v0.4.2) | After (v0.4.4) |
|------|----------------|----------------|
| Tool system | C++ only, 8 hardcoded tools | C++ + Python, auto-discovered via `projv_pytool/` |
| diagram_tool | None | Graphviz DOT renderer with auto-download dot.exe |
| Roles | 3 roles (coder/designer/compactor) | 6 roles (supervisor default, +analyzer, +tester) |
| Coder tools | 9 tools incl. web_search/fetch_url | 7 tools, web/fetch removed |
| Threading doc | v1: ASCII diagrams, StorageWriteQueue | v2.1: 3 PNG diagrams, corrected Storage model |

---

## File Changelog

### New Files
| File | Description |
|------|-------------|
| `src/tools/python_tool_manager.h` | `PythonToolManager` 类声明 |
| `src/tools/python_tool_manager.cpp` | `scanAndRegister` + `executePyTool` + `autoDetectPython` |
| `projv_pytool/__init__.py` | 可空，预留 SDK |
| `projv_pytool/diagram_tool/tool.json` | 图表工具元数据 |
| `projv_pytool/diagram_tool/main.py` | Graphviz DOT 渲染 + `find_dot()` + `download_dot()` |
| `projv_pytool/diagram_tool/requirements.txt` | pip 依赖 |
| `doc/design_pytool.md` | PyTool 完整设计文档 |
| `doc/design_multi_role_prompts.md` | Multi-Role 角色体系设计 |
| `doc/threading_topology.png` | 线程拓扑渲染图 |
| `doc/agent_fsm.png` | Agent 状态机渲染图 |
| `doc/thread_interaction.png` | 线程交汇点渲染图 |
| `.gitignore` | 排除 `__pycache__/` |

### Modified Files
| File | Key Changes |
|------|-------------|
| `src/ui/app.h` | `#include "tools/python_tool_manager.h"` + `std::optional<PythonToolManager> pytoolMgr_` |
| `src/ui/app.cpp` | `setupTools()` 末尾构造 `PythonToolManager` + `scanAndRegister()`；默认角色 `supervisor.md` |
| `src/core/prompts_loader.cpp` | 3 个新 preset（supervisor/analyzer/tester）；coder 工具行精简 |
| `src/tools/md_file_tool.cpp` | 3 处 `std::ios::out` → `std::ios::out \| std::ios::binary` |
| `CMakeLists.txt` | `COMMON_SOURCES` + `HEADERS` 新增 python_tool_manager；POST_BUILD 复制 `projv_pytool/` |
| `doc/threading_model.md` | v2.1 重写：3 张渲染图 + 修正 Storage 模型 |
| `projv_pytool/diagram_tool/*` | 从 Graphviz PATH 依赖 → 自动下载 |

---

## Bugfixes
- **md_file_tool `\r\n` 双回车**: Windows 文本模式下 `ofstream` 自动 `\n` → `\r\n`，当 LLM 传入的 content 已含 `\r\n` 时叠加为 `\r\r\n`。3 处 ofstream 加 `std::ios::binary` 修复。

---

## Upgrade Notes
- 新增角色（supervisor/analyzer/tester）在首次启动时自动创建 `projv_prompts/*.md`。
- 已有 `coder.md` 不会被覆盖——手动删除后重启可获得精简工具清单。
- `diagram_tool` 首次调用时可能下载 ~30MB Graphviz 便携版（需网络），之后离线可用。
- WSL/远程路径下 `workspace_path` 应在 `config.toml` 中显式设置。
