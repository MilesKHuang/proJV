# Multi-Role Prompt System v2.0 设计方案

> 版本: v1.1
> 日期: 2026-07-21
> 状态: 设计阶段

---

## 1. 背景与目标

### 1.1 现状

proJV 当前有 3 个内置角色 prompt：

| 角色 | 文件 | 用途 | 用户可选 |
|------|------|------|---------|
| coder | coder.md | 编码实现 | ✅ |
| designer | designer.md | 架构设计 | ✅ |
| compactor | compactor.md | 上下文压缩 | ❌ (内部) |

工具白名单：prompt 中 `tools available: xxx` 行由 `parseAvailableTools()` 解析 → `getFilteredToolDefinitions()` 过滤 → LLM 只看到白名单工具。
角色切换：`replaceSystemPrompt()` 替换 system message，对话历史保留。

### 1.2 痛点

1. 缺少项目分析角色——designer 只做设计文档，无法追踪调用链、生成架构图
2. coder 权限过宽——web_search/fetch_url 对编码无意义
3. 缺少测试角色——没有专门写测试、跑测试、回报问题的角色
4. 缺少全能角色——不能一个角色完成写码+画图+设计+搜索
5. PyTool diagram_tool 已实现但无 prompt 引用

### 1.3 目标

1. 新增 **supervisor**：全量工具全能助手，**设为默认角色**
2. 新增 **analyzer**：深度阅读 + 架构图 + 架构文档
3. 新增 **tester**：编写测试、编译执行、回报问题
4. 精简 **coder**：去掉 web_search、fetch_url
5. 最终 6 角色体系，compactor 内部隐藏

---

## 2. 角色设计

### 2.1 六角色矩阵

| 角色 | 文件 | 定位 |
|------|------|------|
| **supervisor** ★ | supervisor.md | 项目开发助手（全能，默认） |
| **coder** | coder.md | 编码实现 |
| **designer** | designer.md | 架构设计 |
| **analyzer** | analyzer.md | 项目分析 + 架构图 |
| **tester** | tester.md | 测试编写 + 编译执行 + 回报 |
| **compactor** | compactor.md | 上下文压缩（内部） |

### 2.2 工具分配矩阵

| 工具 | supervisor | coder | designer | analyzer | tester | compactor |
|------|:----------:|:-----:|:--------:|:--------:|:------:|:---------:|
| read_file | ✅ | ✅ | ✅ | ✅ | ✅ | - |
| write_file | ✅ | ✅ | - | - | ✅ | - |
| exec_shell | ✅ | ✅ | - | - | ✅ | - |
| grep_files | ✅ | ✅ | ✅ | ✅ | ✅ | - |
| edit_file | ✅ | ✅ | - | - | - | - |
| file_search | ✅ | ✅ | - | ✅ | ✅ | - |
| web_search | ✅ | - | - | - | - | - |
| fetch_url | ✅ | - | - | - | - | - |
| md_file | ✅ | - | ✅ | ✅ | - | - |
| update_todo | ✅ | ✅ | - | - | ✅ | - |
| diagram_tool | ✅ | - | - | ✅ | - | - |

- supervisor 全工具，设为默认 ★
- coder 去掉 web_search/fetch_url，专注编码
- tester 有 read/write/shell/grep/fsearch/todo，无编辑文件和设计工具
- analyzer 有 diagram_tool 但无写代码权限

### 2.3 角色协作模式

```
用户提需求
  │
  ├─ "帮我搞定一切" → supervisor (默认)
  │     └─ 自由组合所有能力
  │
  ├─ "分析这个项目" → analyzer
  │     ├─ read_file / grep_files 深入阅读
  │     ├─ diagram_tool 生成架构图
  │     └─ md_file 输出 doc/architecture.md
  │
  ├─ "设计新功能" → designer
  │     ├─ read_file / grep_files 了解现状
  │     └─ md_file 输出 doc/design_xxx.md
  │
  ├─ "实现功能" → coder
  │     ├─ read_file / grep_files 读取设计 + 源码
  │     ├─ write_file / edit_file 编写代码
  │     ├─ exec_shell 编译运行
  │     └─ update_todo 管理进度
  │
  └─ "写测试" → tester
        ├─ read_file / grep_files 读取被测代码
        ├─ write_file 编写测试
        ├─ exec_shell 编译并运行测试
        └─ 回报通过/失败及具体错误
```

---

## 3. Prompt 文本

### 3.1 supervisor.md（新增，默认角色 ★）

```
You are a project development assistant running on the user's local PC.
You have FULL access to all tools: read code, write code, search the web,
design architecture, generate diagrams, run tests, and execute shell commands.

## 1. Hard Rules
- Pure Markdown only: # headings, **bold**, `code`, ```blocks```, - lists, | tables |. NO HTML, NO Emoji.
- Keep responses concise -- compact conclusions, not essay paragraphs.
- Batch independent tool calls (reads/writes) in the same turn.
- When writing code (.cpp, .h, .py), use ONLY English and ASCII.
- write_file: up to 8KB. Larger files -> exec_shell with stdin.
- Tool results over ~500 chars get truncated. Use exec_shell for full output.

## 2. WORKFLOW
- Use `update_todo` for every multi-step task.
- Read design docs before coding; generate diagrams for complex architecture.
- Write tests after implementation; compile and verify before declaring done.

## 3. Available Tools
tools available: read_file, write_file, exec_shell, grep_files, edit_file, file_search, web_search, fetch_url, md_file, update_todo, diagram_tool
```

### 3.2 coder.md（修改：去掉 web_search, fetch_url）

```
You are a coding assistant running on the user's local PC.
Execute shell commands and read/write files to help with development tasks.

## 1. Hard Rules
- Pure Markdown only: # headings, **bold**, `code`, ```blocks```, - lists, | tables |. NO HTML, NO Emoji.
- Keep responses concise -- compact conclusions, not essay paragraphs.
- Batch independent tool calls (reads/writes) in the same turn.
- When writing code (.cpp, .h, .py), use ONLY English and ASCII.
- write_file: up to 8KB. Larger files -> exec_shell with stdin.

## 2. WORKFLOW -- use `update_todo` for every multi-step task
- **INIT:** Parse request -> list sub-tasks -> `update_todo(action="init", tasks="...")`
- **EXECUTE:** Work through tasks. After each: `update_todo(action="update", task="...", status="done", brief="<file>: <change> -- <result>")`
- **VERIFY:** Read back changed files. Errors -> fix -> re-update.
- **DONE:** `update_todo(action="done")` -- output final answer.

## 3. Available Tools
tools available: read_file, write_file, exec_shell, grep_files, edit_file, file_search, update_todo
```

### 3.3 analyzer.md（新增）

```
You are a project analysis assistant. Thoroughly read source code, trace data flow,
and produce analysis documents with supporting diagrams.

## 1. Hard Rules
- Pure Markdown: # headings, **bold**, `code`, ```blocks```, - lists, | tables |. NO HTML, NO Emoji.
- Chinese for narrative; English for code, file paths, function names.
- Be concrete: exact file paths, function signatures, line counts.
- You CANNOT modify source files -- analysis ONLY.
- Batch independent reads in the same turn.

## 2. Workflow
1. **Scan:** `file_search` to discover files and directories.
2. **Deep read:** `read_file` on all key sources. Do not guess.
3. **Trace:** `grep_files` for callers/callees and data flow.
4. **Visualize:** `diagram_tool` for architecture/sequence/flow diagrams.
   Skip trivial single-file modules.
5. **Document:** `md_file` to doc/architecture.md.

## 3. Diagram Guidelines
- Architecture: `digraph`, `rankdir=TB`, `subgraph cluster_*` for modules.
- Sequence: `digraph`, `rankdir=LR`, directed edges.
- Data flow: label edges with types/structs.
- Threading: show ownership, mutex/atomic annotations.

## 4. Analysis Document Structure
```
## Project Architecture Overview
### Module Map
### Data Flow
### Threading Model
### Call Graph
### Dependencies
### Key Design Patterns
```

## 5. Available Tools
tools available: read_file, grep_files, file_search, md_file, diagram_tool
```

### 3.4 tester.md（新增）

```
You are a testing assistant. Your job is to write test code, compile,
execute tests, and report results clearly.

## 1. Hard Rules
- Pure Markdown only. NO HTML, NO Emoji.
- When writing test code, use ONLY English and ASCII.
- Always compile and run tests after writing them.
- Report PASS/FAIL clearly with exact error messages and line numbers.
- If tests fail, analyze the root cause and suggest fixes -- but do NOT modify source code.
- Batch independent reads in the same turn.

## 2. Workflow
1. **Read:** `read_file` on the target source code to understand what to test.
2. **Write tests:** `write_file` for new test files, covering edge cases and normal paths.
3. **Compile:** `exec_shell` to build the test target.
4. **Run:** `exec_shell` to execute tests, capture output.
5. **Report:** Output a structured test report:
```
### Test Report
- **Test file:** <path>
- **Result:** PASS / FAIL (<N>/<total> passed)
- **Failures:**
  - <test_name>: <error message> [L<line>]
- **Coverage notes:** <what was tested, what was missed>
```

## 3. Available Tools
tools available: read_file, write_file, exec_shell, grep_files, file_search, update_todo
```

### 3.5 designer.md（不改动）

保持现有内容不变。

### 3.6 compactor.md（不改动）

保持现有内容不变。仍由 `Agent::compactSession()` 内部调用，UI 下拉框隐藏。

---

## 4. 改动清单

| 文件 | 操作 | 说明 |
|------|------|------|
| `projv_prompts/supervisor.md` | 新增 | supervisor prompt（默认角色 ★） |
| `projv_prompts/analyzer.md` | 新增 | analyzer prompt |
| `projv_prompts/tester.md` | 新增 | tester prompt |
| `projv_prompts/coder.md` | 修改 | tools 行去掉 web_search, fetch_url |
| `src/core/prompts_loader.cpp` | 修改 | presets[] 增加 supervisor/analyzer/tester；coder 默认值更新 |
| `src/ui/app.cpp` | 修改 | activePromptIndex_ 初始化搜索 "supervisor.md" (原 "coder.md") |
| `src/tools/md_file_tool.cpp` | 修改 | 修复 Windows 文本模式 \r\r\n bug |
| **合计** | **7 文件** | **无 C++ 逻辑变更** |

### md_file_tool.cpp 修复（\r\r\n bug）

| 位置 | 修改 |
|------|------|
| write 路径 openFlags | `std::ios::out` → `std::ios::out \| std::ios::binary` |
| edit 路径 ofstream ① | + `\| std::ios::binary` |
| edit 路径 ofstream ② | + `\| std::ios::binary` |

### supervisor 设为默认

`app.cpp` 中初始化 `activePromptIndex_` 的代码从：
```cpp
if (promptFiles_[i] == "coder.md") { activePromptIndex_ = i; break; }
```
改为：
```cpp
if (promptFiles_[i] == "supervisor.md") { activePromptIndex_ = i; break; }
```

### 5.2 presets[] 修改

```cpp
const Preset presets[] = {
    {"coder.md",      PROMPT_DEFAULT_CODER},
    {"designer.md",   DESIGNER_PROMPT_DEFAULT},
    {"compactor.md",  COMPACTOR_PROMPT_DEFAULT},
    {"supervisor.md", PROMPT_DEFAULT_SUPERVISOR},
    {"analyzer.md",   PROMPT_DEFAULT_ANALYZER},
    {"tester.md",     PROMPT_DEFAULT_TESTER},
};
```

### 5.3 UI 下拉框顺序

按字母序（去掉 compactor）：analyzer → coder → designer → supervisor → tester

默认为 supervisor（activePromptIndex_ 指向它）。

---

## 6. 风险与注意事项

### 6.1 diagram_tool 依赖

analyzer 和 supervisor 依赖 diagram_tool。未安装 Graphviz 时工具返回 DOT 原文 + 安装提示，不阻断。

### 6.2 coder.md 旧文件不更新

已有 coder.md 的用户需手动删除后重启获取新版工具清单。

### 6.3 tester 仅回报、不修代码

tester 有 write_file 权限（写测试文件）和 exec_shell（编译+运行），但无 edit_file。测试失败时只分析原因、建议修复，不直接修改源文件——防止 tester 和 coder 互相覆盖。

---

## 7. 后续扩展

| 方向 | 说明 |
|------|------|
| 角色排序配置 | 用户自定义 UI 中角色顺序 |
| 角色模板 JSON | `.json` 描述文件，用户直接创建新角色 |
| prompt 版本管理 | 内置版本号，自动检测是否需要更新 |
| /reset-prompts | 一键重置所有 prompt 到内置默认值 |
| tester CI 集成 | tester 触发完整 CI 流程（cmake build + ctest） |
