# Python Tool (PyTool) 设计方案

> 版本: v2.0  
> 日期: 2026-07-21  
> 状态: 设计阶段（可执行方案）

---

## 1. 背景与目标

### 1.1 现状

proJV 当前所有工具均为 C++ 内建，通过 `ToolRegistry` 注册 `ToolExecutor` lambda：

| 工具 | 注册函数 | 类型 |
|------|---------|------|
| shell | `registerShellTool()` | C++ 内建 |
| read_file / write_file / replace_in_file | `registerFileTools()` | C++ 内建 |
| read_md_file / write_md_file | `registerMdFileTool()` | C++ 内建 |
| edit_file | `registerEditFileTool()` | C++ 内建 |
| file_search | `registerFileSearchTool()` | C++ 内建 |
| web_search / web_fetch | `registerSearchTool()` / `registerFetchTool()` | C++ 内建 |

这些工具在 `App::setupTools()` 中一次性注册，无法在运行时动态添加。扩展新能力需要修改 C++ 源码并重新编译。

### 1.2 目标

1. **ToolRegistry 支持注册 Python 工具**：允许以 Python 脚本形式注册工具，与 C++ 内建工具平权
2. **自动发现 `projv_pytool/` 目录**：启动时扫描该目录，自动注册合规的 Python 工具
3. **定义 PyTool 统一接口**：Python 工具通过标准接口与 C++ 上下文交互（workspace、环境变量等）
4. **提供参考实现**：写一个可生成流程图/时序图/架构图的 Python 工具作为端到端验证

### 1.3 设计原则

- **ToolRegistry 零侵入**：不修改 `registry.h/cpp` 的核心接口，只扩展注册能力
- **Python 工具与 C++ 工具平权**：对 Agent 透明，`execute()` 返回格式一致
- **最小依赖**：Python 工具仅依赖 `python` 解释器 + 标准库，可选第三方库由工具自身管理
- **与 exe 同目录**：`projv_pytool/` 固定在 exe 同级目录，与 `projv_prompts/`、`projv_theme/` 一致，不与 workspace 耦合

---

## 2. 架构设计

### 2.1 新增组件

```
proJV
 ├── ToolRegistry (existing)
 │    ├── C++ tools (existing)
 │    └── PythonToolManager (new)    ← 管理所有 Python 工具的注册与执行
 │         ├── scanAndRegister()     ← 扫描 {exeDir}/projv_pytool/ 目录
 │         └── executePyTool()       ← 执行 Python 工具（static）
 ├── projv_pytool/                   ← Python 工具存放目录（exe 同级）
 │    ├── __init__.py                ← 保留，当前为空
 │    ├── diagram_tool/              ← 参考实现：图表生成工具
 │    │    ├── tool.json
 │    │    ├── main.py
 │    │    └── requirements.txt
 │    └── ... (更多工具)
 └── Agent (existing, no changes)
```

### 2.2 执行流程

```
Agent::run() → 解析 LLM 返回的 tool_call
  → ToolRegistry::execute(name, args)
    → 查找 executors map
      ├── 若为 C++ 工具 → 直接执行 lambda
      └── 若为 Python 工具 → PythonToolManager::executePyTool()
           ├── 将 args 通过 stdin 传入 Python 进程（避免命令行长度限制）
           ├── 设置环境变量:
           │    PROJV_WORKSPACE=<workspacePath>
           │    PROJV_TOOL_DIR=<tool_dir>
           ├── 构造命令行: python <tool_dir>/main.py
           ├── 执行 subprocess（同步等待）
           ├── 捕获 stdout，截断至 32000 字符
           ├── 退出码非 0 时：返回 "[PyTool Error] <stderr>"
           └── 返回结果字符串
```

### 2.3 `projv_pytool/` 路径解析

与 `projv_prompts/` 完全一致：**固定为 exe 所在目录下的 `projv_pytool/`**。

```cpp
// 路径计算方式（参考 prompts_loader.cpp 的 promptsDir()）：
std::string pytoolDir() {
    std::string configPath = getConfigPath();
    auto parent = std::filesystem::path(configPath).parent_path();
    return (parent / "projv_pytool").string();
}
```

不与 `config.workspacePath` 耦合：workspace 是 Agent 执行文件操作的工作目录，与工具注册是两个独立概念。

---

## 3. 核心设计

### 3.1 PythonToolManager 接口

```cpp
class PythonToolManager {
public:
    // 构造时存下 pythonPath 和 workspacePath，供 lambda 捕获
    PythonToolManager(std::string pythonPath, std::string workspacePath);

    // 扫描 {exeDir}/projv_pytool/，注册合规工具
    void scanAndRegister(ToolRegistry& registry);

    // 同步执行 Python 工具，返回 stdout（或错误信息）
    static std::string executePyTool(
        const std::string& toolDir,
        const std::string& pythonPath,
        const std::string& workspacePath,
        const std::string& args);

private:
    std::string pythonPath_;      // python 解释器路径（已做 auto-detection）
    std::string workspacePath_;   // 传给 PROJV_WORKSPACE 环境变量
};
```

### 3.2 目录规范

```
projv_pytool/                    ← 固定位置：{exeDir}/projv_pytool/
 ├── __init__.py                 ← 保留，当前为空（为未来 projv_toolkit SDK 预留）
 ├── <tool_name>/                ← 目录名即工具名
 │    ├── tool.json              ← 工具元数据（必选）
 │    ├── main.py                ← 工具入口（必选）
 │    ├── requirements.txt       ← 依赖声明（可选）
 │    └── ...                    ← 辅助模块/资源文件（可选）
 └── ...
```

### 3.3 tool.json 元数据格式

```json
{
    "name": "工具名（必须与目录名一致，不一致时以目录名为准并输出 warning）",
    "description": "工具描述，LLM 据此判断何时调用",
    "parameters": [
        {
            "name": "参数名",
            "type": "string | number | boolean | array",
            "description": "参数说明，越详细 LLM 用得越准",
            "required": true | false
        }
    ]
}
```

**`type` 字段说明**：`tool.json` 中的 `type` 值直接透传给 `ToolParameter::type`，再由 `ToolDefinition::toJsonSchema()` 转为 JSON Schema 的 `type` 字段。支持的值：`"string"`、`"number"`、`"boolean"`、`"array"`。

### 3.4 Python 工具统一接口

每个工具的 `main.py` 必须实现以下协议：

```
调用方式:
    python <tool_dir>/main.py

stdin:
    接收 JSON 参数字符串（由 C++ 端将 args 通过管道写入 stdin）

环境变量:
    PROJV_WORKSPACE  - 工作区绝对路径
    PROJV_TOOL_DIR   - 当前工具目录绝对路径

成功:
    stdout 输出结果文本（纯文本或 JSON），退出码 0

失败:
    stderr 输出错误信息，退出码非 0
    C++ 端将 stderr 作为 "[PyTool Error] <stderr>" 返回
```

**设计理由：使用 stdin 而非 `--args`**：避免 Windows cmd.exe 8191 字符命令行上限。工具参数（如 diagram_tool 的 `code` 字段）可能包含大量文本，通过 stdin 传入无长度限制。

入口代码骨架：

```python
import sys, json, os

def run(args: dict) -> str:
    """工具主逻辑，args 来自 LLM 的参数填充"""
    # ... 工具实现 ...
    return "result"

def main():
    try:
        raw = sys.stdin.read()
        args = json.loads(raw)
        result = run(args)
        print(result)
    except json.JSONDecodeError as e:
        print(f"Error: invalid JSON args: {e}", file=sys.stderr)
        sys.exit(1)
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)

if __name__ == '__main__':
    main()
```

**依赖检查模板**：使用第三方库的工具应在 `run()` 开头做 Import 检查：

```python
def run(args: dict) -> str:
    try:
        import graphviz  # 第三方依赖
    except ImportError:
        return (
            "Error: 'graphviz' package not installed.\n"
            "Run: pip install graphviz"
        )
    # ... 正常逻辑 ...
```

### 3.5 注册流程

```
scanAndRegister():
  1. 确定 projv_pytool/ 路径（固定: {exeDir}/projv_pytool/，见 2.3）
  2. 若 pythonPath_ 为空 → auto-detection（见 6.1）
     失败则跳过所有工具，输出 "[PyTool] Python not found, skipping scan"
  3. 遍历第一层子目录:
     a. 检查 tool.json 存在、JSON 合法、必填字段完整 → 否则 log + skip
     b. 检查 main.py 是否存在 → 否则 log + skip
     c. 若 tool.json.name != 目录名 → log warning, 以目录名为准
     d. 注册到 ToolRegistry:
          tools.registerTool(
              ToolDefinition{name, description, parameters},
              executor = lambda: executePyTool(toolDir, pythonPath_, workspacePath_, args)
          )
     e. 日志: "[PyTool] Registered '<name>' from <toolDir>"
```

Lambda 捕获三个必要变量：

```cpp
[toolDir, pythonPath = pythonPath_, workspacePath = workspacePath_]
(const std::string& args) -> std::string {
    return PythonToolManager::executePyTool(
        toolDir, pythonPath, workspacePath, args);
}
```

---

## 4. 改动清单

| 文件 | 操作 | 说明 | 约行数 |
|------|------|------|--------|
| `src/tools/python_tool_manager.h` | 新增 | PythonToolManager 类声明 | 40 |
| `src/tools/python_tool_manager.cpp` | 新增 | scanAndRegister + executePyTool + pytoolDir + autoDetectPython | 140 |
| `projv_pytool/__init__.py` | 新增 | 保留，当前为空 | 1 |
| `projv_pytool/diagram_tool/tool.json` | 新增 | 图表工具元数据 | 30 |
| `projv_pytool/diagram_tool/main.py` | 新增 | 图表工具实现（stdin 传入） | 210 |
| `projv_pytool/diagram_tool/requirements.txt` | 新增 | 依赖声明 | 1 |
| `src/ui/app.h` | 修改 | 添加 `#include "tools/python_tool_manager.h"`<br>添加 `PythonToolManager pytoolMgr_` 成员 | +3 |
| `src/ui/app.cpp` | 修改 | `setupTools()` 末尾构造 PythonToolManager<br>并调用 `scanAndRegister()` | +5 |
| `CMakeLists.txt` | 修改 | `COMMON_SOURCES` 加 `python_tool_manager.cpp`<br>`HEADERS` 加 `python_tool_manager.h` | +2 |
| **合计** | **9 文件** | | **~432 行** |

**执行顺序保证**：`scanAndRegister()` 在 `setupTools()` 中同步调用，早于 Agent 线程启动。注册完成后 `ToolRegistry` 只读使用，无并发写竞争。

---

## 5. 参考实现：diagram_tool 工具

### 5.1 设计思路

选择图表生成作为第一个 PyTool 参考实现的原因：

- **LLM 天然擅长**：流程图/架构图本质是结构化文本（DOT 语言），LLM 生成 DOT 代码质量高
- **Python 生态成熟**：`graphviz` 库可将 DOT 渲染为 PNG/SVG，Windows 友好
- **实用价值高**：开发 GUI Agent 过程中频繁需要画架构图、流程图梳理逻辑
- **展示 PyTool 价值**：C++ 内建工具完全无法做到，封装了安装检测、格式转换、错误处理

### 5.2 参数设计

工具使用 **Graphviz DOT 语言** 作为图表描述格式。

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `code` | string | 是 | Graphviz DOT 格式的图表描述代码 |
| `output` | string | 否 | 输出文件名（不含扩展名），默认 `diagram` |
| `format` | string | 否 | 输出格式：`png` / `svg`，默认 `png` |

**`code` 字段传入方式**：通过 **stdin** 传入 JSON 参数串（见 3.4），避免 Windows 命令行 8191 字符上限。工具内部将 `code` 写入临时 `.dot` 文件，调用 Graphviz 渲染。

**DOT 语言能力**：LLM 可在 `code` 中使用全部 DOT 语法 — `digraph`/`graph`、`rankdir`、`shape`（box/diamond/ellipse/record/note）、`subgraph cluster_*`、`label`、`style`/`fillcolor`/`color`。工具不做语法校验，只负责透传。

渲染失败时返回 DOT 原文 + 错误信息，LLM 仍可读取代码理解图表结构。

### 5.3 工具行为

按以下优先级尝试渲染：

1. **Graphviz**（`dot` 命令行）：输出 PNG/SVG，需系统安装 Graphviz
2. **Python graphviz 库**：纯 Python 实现，无需系统安装

都不可用时，返回 DOT 代码原文 + 安装提示。

### 5.4 依赖

```
# 主方案：系统安装 Graphviz（渲染效果最好）
# Windows: choco install graphviz / scoop install graphviz / 手动安装

# 回退方案：纯 Python 实现
pip install graphviz
```

---

## 6. 风险与注意事项

### 6.1 Python 解释器路径

`config.pythonPath` 在 `config.toml` 中配置。`PythonToolManager` 构造函数中做 auto-detection：

1. 若 `config.pythonPath` 非空 → 直接使用
2. 若为空 → 依次尝试 `python` / `python3` / `py`，用 `where` 解析绝对路径
3. 都失败 → `pythonPath_` 置空，`scanAndRegister` 跳过所有工具并输出 `[PyTool] Python not found, skipping scan`

### 6.2 安全性

- 执行通过 subprocess，与 shell tool 同级风险
- 工具脚本路径固定为 `{exeDir}/projv_pytool/<name>/main.py`，防止注入
- 参数通过 stdin 传入而非命令行拼接，避免 shell 注入
- DOT 代码中的外部文件引用由 LLM 生成，本地使用场景下风险可控

### 6.3 性能

每次工具调用启动一个 Python 子进程，约 50-200ms 启动开销。Graphviz 渲染本身毫秒级。

### 6.4 输出截断

`executePyTool` 对 stdout 截断：超过 32000 字符时截取前 32000 字符并追加 `\n... (truncated)`。与 shell_tool 一致。

### 6.5 错误返回

- 退出码 0 → 返回 stdout
- 退出码非 0 → 返回 `"[PyTool Error] <stderr>"` （stderr 为空时返回 `"[PyTool Error] exit code: N"`）
- 进程启动失败 → 返回 `"[PyTool Error] Failed to start: <win32 error>"`

### 6.6 依赖管理

每个工具可在目录下放置 `requirements.txt`。`main.py` 应在 `run()` 开头做 Import 检查（见 3.4 模板），缺失时返回友好安装提示。

### 6.7 线程安全

`scanAndRegister()` 在 `App::setupTools()` 中调用，早于 Agent 线程启动。注册完成后 `ToolRegistry::executors` map 仅被 Agent 线程读取。`executePyTool` 每次调用创建独立子进程，无共享状态。

---

## 7. 扩展方向（后续）

| 方向 | 说明 |
|------|------|
| Python 工具热加载 | 运行时重新扫描 `projv_pytool/`，无需重启 |
| 常驻 Python 进程 | 减少子进程启动开销，支持有状态工具 |
| 工具依赖自动安装 | 检测 `requirements.txt`，自动 `pip install` |
| PyTool SDK | 提供 `projv_toolkit` Python 包，封装上下文获取、日志 |
| 沙箱执行 | 对不可信 Python 工具使用容器/虚拟环境隔离 |

---

## 8. 推荐 PyTool 清单（后续可扩展）

| 优先级 | 工具 | 说明 | 依赖 | 可用性 |
|--------|------|------|------|--------|
| ⭐⭐⭐⭐⭐ | `diagram_tool` | 流程图/架构图（参考实现） | graphviz | ✅ 纯本地 |
| ⭐⭐⭐⭐⭐ | `stock_query` | A 股/港股/美股行情与走势 | akshare | ✅ 国内数据源 |
| ⭐⭐⭐⭐ | `news_summary` | 聚合 RSS 摘要 | feedparser | ✅ RSSHub |
| ⭐⭐⭐⭐ | `run_python` | Python 代码片段执行 | 无 | ✅ 纯本地 |
| ⭐⭐⭐⭐ | `arxiv_fetch` | 搜索/下载 arXiv 论文 | arxiv, PyMuPDF | ✅ 可用 |
| ⭐⭐⭐ | `read_excel` | 读取 Excel/CSV | openpyxl | ✅ 纯本地 |
| ⭐⭐⭐ | `read_docx` | 读取 Word 文档 | python-docx | ✅ 纯本地 |
| ⭐⭐⭐ | `qr_code` | 生成二维码图片 | qrcode, Pillow | ✅ 纯本地 |
| ⭐⭐ | `weather` | 查询天气 | requests | ✅ 和风天气 |
| ⭐⭐ | `currency` | 汇率换算 | requests | ✅ 可用 |

所有工具不需要自己维护服务器，数据源为公开 API 或纯本地操作。
