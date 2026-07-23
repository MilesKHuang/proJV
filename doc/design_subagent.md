# Sub-Agent 架构设计方案 v2.2

> 版本: v2.2
> 日期: 2026-07-23
> 状态: 设计阶段
> 前置阅读: `design_multi_role_prompts.md`, `threading_model.md`

---

## 1. 背景与目标

### 1.1 当前架构现状

proJV 当前为**单 Agent + 多角色 Prompt** 架构，整个项目生命周期共 **3 个线程**：

| 线程 | 位置 | 生命周期 |
|------|------|----------|
| UI 主线程 | `main.cpp` WinMain 消息循环 | 常驻 |
| Agent 工作线程 (`agentThread_`) | `app.h:119` → `app.cpp:846` | 每次用户输入时创建，完成后回收 |
| 模型获取线程 (`fetchModelsThread_`) | `app.h:121` → `app.cpp:314` | 初始化时短暂运行 |

**当前 Agent 核心结构** (`src/core/agent.h`):

```
Agent
 ├── DeepSeekClient& client          (引用，与 App 共享)
 ├── ToolRegistry& toolRegistry      (引用，与 App 共享)
 ├── Storage* storage                (指针，可空)
 ├── Session session                 (owned，有 mtx 保护)
 ├── cancelRequested_                (std::atomic<bool>，owned)
 ├── todoData + todoMutex            (owned)
 ├── systemPrompt_ + allowedTools_   (角色切换机制)
 └── AgentPhase 状态机               (Idle → Streaming → ExecutingTools → AwaitApproval → Error)
```

**当前多角色体系** (已实现，见 `src/core/prompts_loader.cpp`):

| 角色 | 文件 | 工具权限 | 用户可见 | 分类 |
|------|------|----------|---------|------|
| **supervisor** ★ | supervisor.md | 全量 11 工具 | ✅ 默认 | **Supervisor** |
| coder | coder.md | 7 工具 (编码) | ✅ | Sub-Agent |
| designer | designer.md | 3 工具 (只读+md) | ✅ | Sub-Agent |
| analyzer | analyzer.md | 5 工具 (分析+图) | ✅ | Sub-Agent |
| tester | tester.md | 6 工具 (测试) | ✅ | Sub-Agent |
| compactor | compactor.md | 无工具 | ❌ 内部 | Internal |

**角色切换机制** (已实现):
- `Agent::setSystemPrompt()` → 设置 `systemPrompt_` + 调用 `reloadAllowedTools()` 解析白名单
- `Agent::replaceSystemPrompt()` → 替换 Session 中第一条 system message，保留对话历史
- `parseAvailableTools()` → 从 prompt 文本解析 `tools available: xxx` 行
- `getFilteredToolDefinitions()` → 按白名单过滤 ToolRegistry 中的工具定义

### 1.2 痛点

1. **单 Agent 瓶颈**：所有任务串行执行，无法并行处理独立子任务（如同时代码搜索 + 架构分析）
2. **角色切换丢失上下文**：切换角色时 `replaceSystemPrompt()` 替换 system message，但 Agent 状态机、todoData、toolCallDepth_ 等不重置，可能导致状态污染
3. **无委派机制**：supervisor 虽有全量工具，但无法将子任务委派给专用角色（如"让 analyzer 分析这个模块，让 tester 跑测试"）
4. **Cancel 粒度不足**：`cancelRequested_` 是单值 `atomic<bool>`，无法区分"取消当前 tool call"和"取消整个 Agent"
5. **无 SubAgent 生命周期管理**：没有创建、监控、回收子 Agent 的机制
6. **UI 无 SubAgent 可见性**：用户看不到子任务执行状态

### 1.3 目标

1. **Supervisor 为核心入口**：用户只与 Supervisor 交互，Supervisor 负责意图分析、任务分解、委派调度、结果验收
2. **Sub-Agent 为执行单元**：coder/designer/analyzer/tester 作为独立 SubAgent 实例，由 Supervisor 按需创建和回收
3. **Compactor 保持内部**：不可被用户或 Supervisor 直接调用，仅由 Agent 内部 `compactSession()` 触发
4. **统一 Cancel 机制**：一次取消终止所有 tool call、shell 子进程、所有活跃 SubAgent，保证线程安全
5. **稳健的分级调度**：Supervisor 不胡乱起 SubAgent，基于任务类型和复杂度做判断
6. **UI 监控面板**：显示 SubAgent 执行状态与存活情况
7. **工作流配置驱动**：通过 `projv_workflows/` 目录下的 JSON 文件定义完整工作流，用户可创建多套方案并随时切换

### 1.4 设计原则

- **Agent 核心最小改动**：`Agent::run()` 状态机（Streaming → ExecutingTools → Idle 循环）保持不变
- **Supervisor 是 Agent 的超集**：Supervisor 继承/组合 Agent，增加调度能力
- **SubAgent 是 Agent 的薄封装**：组合 Agent + 独立 DeepSeekClient + 独立 ToolRegistry + 受限工具集
- **最小依赖**：SubAgent 不需要 Storage、不需要 UI 渲染、不需要 Approval 交互、不需要 todo tool
- **SubAgent 使用廉价模型**：默认使用 flash 等低成本模型，降低并行调用开销
- **并行优先**：多个 SubAgent 可同时执行，Supervisor 用 `when_all` 等待结果
- **SubAgent 可跨轮复用**：默认不杀死已完成的 SubAgent，下一轮对话可查询状态并复用上下文
- **并发上限**：同时活跃 SubAgent 上限为 5 个，超出时排队等待

---

## 2. 角色体系与 System Prompt 设计

### 2.1 角色分类

```
┌─────────────────────────────────────────────────────┐
│                    Supervisor                        │
│  · 用户唯一交互入口                                   │
│  · 全量工具 (12 tools，含 delegate_task)              │
│  · 意图分析 → 任务分解 → 委派调度 → 结果验收           │
│  · 自身也可直接执行简单任务                            │
└──────────┬──────────┬──────────┬──────────┐
           │          │          │          │
    ┌──────▼──┐ ┌─────▼───┐ ┌───▼────┐ ┌───▼────┐
    │  Coder  │ │Designer │ │Analyzer│ │ Tester │
    │ 7 tools │ │ 3 tools │ │5 tools │ │6 tools │
    │  flash  │ │  flash  │ │ flash  │ │ flash  │
    └─────────┘ └─────────┘ └────────┘ └────────┘

    ┌──────────────────────────────────────┐
    │           Compactor (Internal)        │
    │  · 不可被用户/Supervisor 直接调用      │
    │  · 仅由 Agent::compactSession() 触发  │
    └──────────────────────────────────────┘
```
### 2.2 System Prompt 区分原则

| 角色 | Prompt 核心指令 | 关键差异 |
|------|---------------|---------|
| **Supervisor** | 你是项目开发助手，拥有全量工具。分析用户意图，将复杂任务委派给专业 SubAgent，验收结果后回复用户。 | 调度 + 验收 + 全工具 |
| **Coder** | 你是编码助手。读取设计文档和源码，编写/修改代码，编译验证。 | 专注编码实现 |
| **Designer** | 你是设计助手。深入阅读源码，产出结构化设计文档。不可修改源码。 | 只读 + 设计输出 |
| **Analyzer** | 你是分析助手。追踪调用链、数据流，生成架构图和文档。不可修改源码。 | 分析 + 图表 |
| **Tester** | 你是测试助手。编写测试代码，编译执行，回报结果。不修改被测源码。 | 测试 + 回报 |
| **Compactor** | 你是上下文压缩助手。将历史对话总结为结构化摘要。 | 纯内部，无工具 |

**Compactor 说明**：Compactor 不纳入工作流 JSON 的 `subagents` 列表，不由 Supervisor 委派。它仅由 `Agent::compactSession()` 内部调用，使用 `compactor.md` 作为 prompt。用户仍可修改 `projv_prompts/compactor.md` 来定制压缩行为，但这不影响任何工作流配置。

### 2.3 Supervisor Prompt 设计要点

Supervisor 的 system prompt（`supervisor.md`）在现有基础上增强，**必须保留原有的 Hard Rules、Workflow、Available Tools 三段落**。具体 prompt 文本由实现者根据以下要点撰写，不在此文档中完整列出。

**段落结构**：

| 段落 | 内容要点 |
|------|---------|
| **1. Hard Rules** | 保持与现有 `supervisor.md` 一致：Pure Markdown、简洁回复、批量 tool call、ASCII only 代码、write_file 8KB 限制 |
| **2. WORKFLOW** | 以 **Analyze → Design → Implement → Test** 四阶段为执行框架。每个阶段说明何时委派给对应 SubAgent（analyzer/designer/coder/tester）。使用 `update_todo` 跟踪进度 |
| **3. Available Tools** | 现有 11 工具 + 新增 `delegate_task`，共 12 工具。`tools available:` 行需包含 delegate_task |
| **4. Sub-Agent Delegation Rules** | 见下方详细设计 |

**第 4 段 Delegation Rules 必须包含的内容**：

1. **复杂度阈值**（与 4.3 节一致）：
   - 低（≤1 文件，≤50 行）→ 直接执行
   - 中（>3 文件或 >250 行）→ 可选委派
   - 高（>5 文件或 >500 行）→ 必须委派

2. **委派时机**：多模块分析→analyzer、架构设计→designer、多文件实现→coder、测试套件→tester

3. **不委派时机**：单文件小改、简单问答、用户明确要求

4. **委派流程**：
   - 分析请求 → 估算复杂度
   - 调用 `delegate_task action=list` 检查现有 SubAgent
   - 按 6.4 节三步决策（必须释放 → 优先复用 → 按需释放）处理现有 SubAgent
   - 匹配则 `reuse_id` 复用，否则 `action=start` 创建
   - 用 `action=status` 轮询进度
   - 验收结果，不满足则重新委派
   - 整合结果回复用户

5. **可用 SubAgent 类型**：coder/designer/analyzer/tester（从当前工作流 JSON 的 `subagents` 中动态生成）

6. **结果处理**：SubAgent 回复应简洁（通过角色 prompt 中的 "keep responses concise" 约束实现），复杂结果输出为 `doc/` 目录下的 `.md` 报告文件

**关键约束**：
- 第 1-3 段保持与现有 `supervisor.md` 结构一致，仅 Workflow 改为四阶段、Available Tools 新增 delegate_task
- 第 4 段为新增，指导 LLM 何时使用 `delegate_task` 工具
- 不在此文档中写完整 prompt 文本，由实现者根据上述要点撰写

### 2.4 SubAgent Prompt 注入策略

SubAgent 启动时，注入两层 system message：

```
[Layer 1] 角色 Prompt (如 coder.md，包含该角色的 Hard Rules + Workflow + Available Tools)
    ↓
[Layer 2] 任务上下文 (Supervisor 动态生成)
    "You are working on a sub-task delegated by the Supervisor.
     Task: <description>
     Context: <relevant file paths, constraints, expected output>
     Report back when done. Do NOT ask the user questions — the Supervisor will handle that."
```

### 2.5 SubAgent 模型选择

所有 SubAgent 默认使用 **flash 级别廉价模型**（如 `deepseek-v4-flash`），降低并行调用成本。Supervisor 自身可使用 pro 级别模型。

| Agent | 默认模型 | 可配置 |
|-------|---------|--------|
| Supervisor | `deepseek-v4-pro`（用户可选） | 是 |
| SubAgent (所有类型) | `deepseek-v4-flash` | 是（配置文件中指定） |

---

## 3. 工作流配置设计

### 3.1 工作流目录 `projv_workflows/`

在配置目录下新增 `projv_workflows/` 目录，存放多个工作流 JSON 文件。每个 JSON 文件是一个**完整的工作流方案**，定义 main_agent 和 subagent_list。程序启动时扫描此目录；若目录为空或不存在，自动生成预设 `coding.json`。

**原有 UI 下拉选 system_prompts 改为对此目录下 JSON 文件的选取**：UI 不再扫描 `projv_prompts/` 目录下的 `.md` 文件，而是列出 `projv_workflows/` 目录下所有 `.json` 文件。用户通过下拉菜单切换不同的工作流方案，每个方案是一套完整的 main_agent + subagent_list 配置。

**预设工作流文件 `projv_workflows/coding.json`**（目录为空时自动生成）：
```json
{
  "version": "1.0",
  "description": "Default coding workflow — full development cycle with analysis, design, implementation, and testing",

  "main_agent": {
    "prompt_file": "supervisor.md",
    "model": ""                      // 空字符串 = fallback 到 config.toml 中的 model 配置
  },

  "subagents": [
    {
      "name": "coder",
      "display": "Coder",
      "prompt_file": "coder.md",
      "description": "Code implementation, compilation, debugging",
      "model": "deepseek-v4-flash",
      "visible": true
    },
    {
      "name": "designer",
      "display": "Designer",
      "prompt_file": "designer.md",
      "description": "Architecture design documents",
      "model": "deepseek-v4-flash",
      "visible": true
    },
    {
      "name": "analyzer",
      "display": "Analyzer",
      "prompt_file": "analyzer.md",
      "description": "Code analysis, call tracing, diagram generation",
      "model": "deepseek-v4-flash",
      "visible": true
    },
    {
      "name": "tester",
      "display": "Tester",
      "prompt_file": "tester.md",
      "description": "Test writing, compilation, execution, reporting",
      "model": "deepseek-v4-flash",
      "visible": true
    },
  ]
}
```

**用户可创建多套工作流**，例如：
- `projv_workflows/coding.json` — 完整开发工作流（预设）
- `projv_workflows/coding_simple.json` — 精简版，只有 coder + tester
- `projv_workflows/review.json` — 代码审查工作流，只有 analyzer + reviewer
- `projv_workflows/my_custom.json` — 用户自定义的任意组合

UI 下拉菜单列出所有 `.json` 文件名（去掉扩展名作为显示名），用户切换即加载对应工作流方案。

### 3.2 加载流程

```
App::initialize()
    │
    ├─ 1. 扫描 <config_dir>/projv_workflows/ 目录下所有 .json 文件
    │     ├─ 目录存在且有 .json 文件 → 列出所有文件名
    │     └─ 目录不存在或为空 → 创建目录 → 生成预设 coding.json → 使用预设
    │
    ├─ 2. 构建 UI 下拉菜单：
    │     └─ 列出所有 .json 文件名（去掉扩展名作为显示名）
    │     └─ 默认选中上次使用的文件（记录在 config.toml 中），若无记录则选第一个
    │
    ├─ 3. 加载用户选中（或默认）的 JSON 文件：
    │     ├─ 解析 JSON → 提取 main_agent 和 subagents
    │     ├─ 对每个 subagent：
    │     │   ├─ 读取 prompt_file 指向的 .md 文件
    │     │   ├─ 解析 "tools available:" 行 → 提取工具白名单
    │     │   └─ 构建 AgentTypeInfo 注册到 AgentRegistry
    │     └─ 加载 main_agent.prompt_file 作为 Supervisor 的初始 system prompt
    │
    └─ 4. 验证：
          ├─ main_agent.prompt_file 必须存在
          ├─ 每个 subagent.prompt_file 必须存在
          └─ 至少有一个 visible=true 的 subagent
```

**用户切换工作流时**：
- UI 下拉菜单选择新 JSON → 调用 `loadWorkflow(jsonPath)` → 重新解析 JSON → 重建 AgentRegistry → `replaceSystemPrompt()` 替换 Supervisor 的 system prompt
- 对话历史保留，仅 system prompt 和可用 SubAgent 类型变更

### 3.3 Fallback 机制

当 `projv_workflows/` 目录不存在或为空时，程序自动创建目录并生成预设 `coding.json`（内容与 3.1 节示例一致），然后使用该预设值继续运行。

**实现重点**：
- `prompts_loader.cpp` 新增 `scanWorkflows()` 函数，返回目录下所有 `.json` 文件名列表
- `prompts_loader.cpp` 新增 `loadWorkflow(jsonPath)` 函数，解析单个 JSON 并返回配置结构体
- 配置结构体包含 `MainAgentConfig` 和 `std::vector<SubAgentConfig>`
- 目录不存在时：创建目录 → 生成预设 `coding.json` → log info → 加载预设
- 单个 JSON 解析失败时：log warning → 跳过该文件
- 用户修改 JSON 后通过 UI 下拉菜单即时切换（无需重启）

### 3.4 用户扩展方式

用户可自由创建多套工作流方案，只需两样东西：

1. **Prompt 文件**：在 `projv_prompts/` 目录创建 `.md` 文件（包含 `tools available: xxx` 行定义工具白名单）
2. **工作流 JSON**：在 `projv_workflows/` 目录创建新的 `.json` 文件，引用 prompt 文件并定义 subagent 组合

**示例：创建代码审查工作流 `review.json`**：

```json
{
  "version": "1.0",
  "description": "Code review workflow — analyze and review only, no code modification",

  "main_agent": {
    "prompt_file": "supervisor.md",
    "model": ""
  },

  "subagents": [
    {
      "name": "analyzer",
      "display": "Analyzer",
      "prompt_file": "analyzer.md",
      "description": "Deep code analysis and tracing",
      "model": "deepseek-v4-flash",
      "visible": true
    },
  ]
}
```

对应的 `reviewer.md` 需包含 `tools available: read_file, grep_files, file_search, md_file` 等行。

用户通过 UI 下拉菜单在 `coding`、`review`、`coding_simple` 等工作流之间切换，每个工作流激活不同的 SubAgent 组合。

---

## 4. Supervisor 调度机制

### 4.1 调度决策流程

```
用户输入
    │
    ▼
┌──────────────────────────────────────┐
│ Supervisor 接收用户消息               │
│ (与普通 Agent 相同的 startTurn/run)   │
└──────────────┬───────────────────────┘
               │
               ▼
┌──────────────────────────────────────┐
│ LLM 分析意图 + 估算复杂度             │
│ · 涉及文件数？预估改动行数？           │
│ · 是否需要委派？                      │
└──────────────┬───────────────────────┘
               │
       ┌───────┴────────┐
       │                │
   简单任务              复杂任务
  (≤1 文件,             (>3 文件 或
   ≤50 行改动)           >250 行改动)
       │                │
       ▼                ▼
┌──────────┐   ┌──────────────────────────┐
│ 直接执行  │   │ 调用 delegate_task        │
│ (现有流程)│   │ action=list               │
└──────────┘   │ 检查现有 SubAgent 状态      │
               └──────────┬───────────────┘
                          │
              ┌───────────┴───────────┐
              │                       │
         有可复用                  无可复用
         SubAgent                 SubAgent
              │                       │
              ▼                       ▼
   ┌──────────────────┐   ┌──────────────────────┐
   │ delegate_task     │   │ delegate_task         │
   │ action=start      │   │ action=start          │
   │ reuse_id=<id>     │   │ agent_type=角色       │
   │ (复用已有上下文)   │   │ task=任务描述         │
   └────────┬─────────┘   └──────────┬───────────┘
            │                        │
            └──────────┬─────────────┘
                       │
                       ▼
            ┌──────────────────────┐
            │ SubAgentManager      │
            │ · 检查并发数 < 5      │
            │ · 创建/复用 SubAgent  │
            │ · 注入 prompt + 上下文│
            │ · 启动独立线程        │
            └──────────┬───────────┘
                       │
                       ▼
            ┌──────────────────────┐
            │ SubAgent 并行执行     │
            │ · 各自独立 run() 循环 │
            │ · 完成后返回结果       │
            └──────────┬───────────┘
                       │
                       ▼
            ┌──────────────────────┐
            │ Supervisor 验收结果   │
            │ · 检查是否满足要求     │
            │ · 不满足 → 重新委派   │
            │ · 满足 → 整合回复     │
            │ · 保留 SubAgent 供复用 │
            └──────────────────────┘
```

### 4.2 delegate_task 工具设计

**工具名**：`delegate_task`

**关键参数**：

| 参数 | 类型 | 说明 |
|------|------|------|
| `action` | enum: `"start"`, `"status"`, `"cancel"`, `"list"`, `"reap"` | 操作类型 |
| `agent_type` | string | SubAgent 类型名，action=start 时必填 |
| `task` | string | 自包含的任务描述，含目标、约束、期望输出格式、相关文件路径，action=start 时必填 |
| `agent_id` | string | SubAgent 实例 ID，action=status/cancel/reap 时必填 |
| `reuse_id` | string | 要复用的已有 SubAgent ID，action=start 时可选。若提供，跳过创建直接复用 |

**action=start 返回**：`{ agent_id, status: "running" }`
**action=status 返回**：`{ agent_id, status, phase, current_tool, progress, elapsed_ms }`
**action=cancel 返回**：`{ agent_id, status: "cancelled" }`
**action=list 返回**：`{ agents: [{ agent_id, type, status, task_summary, idle }] }`
**action=reap 返回**：`{ agent_id, status: "reaped" }`（按 ID 析构指定 SubAgent）

**实现重点**：
- `delegate_task` 工具注册到 Supervisor 的 ToolRegistry（不在 SubAgent 的工具集中）
- executor 持有 `SubAgentManager&` 引用，直接调用其 spawn/cancel/getStatus/reap 方法
- action=start 时同步返回 agent_id（不等待 SubAgent 完成），Supervisor 后续用 action=status 轮询
- action=list 让 Supervisor 在新一轮对话开始时了解现有 SubAgent 状态，判断哪些可复用
- action=reap 让 Supervisor 显式按 ID 析构不再需要的 SubAgent
- `reuse_id` 参数允许 Supervisor 直接复用已有 SubAgent 的上下文，避免重复创建

### 4.3 分级调用策略

Supervisor 的 LLM prompt 中嵌入以下决策规则，确保不胡乱起 SubAgent：

**复杂度阈值**（替代简单的 tool call 计数）：

| 复杂度 | 判定标准 | 决策 |
|--------|---------|------|
| 低 | 涉及 ≤1 文件，预估改动 ≤50 行 | **直接执行** |
| 中 | 涉及 >3 文件，或预估改动 >250 行 | **可选委派**（视任务类型决定）。此规模相当于一个中型 module |
| 高 | 涉及 >5 文件，或预估改动 >500 行 | **必须委派**。此规模相当于一个小型 system |

**场景决策表**：

| 场景 | 决策 | 理由 |
|------|------|------|
| 单文件小改 / 简单问答 | **直接执行** | 复杂度低，委派开销 > 收益 |
| 多模块代码分析 + 文档 | **委派给 analyzer** | 专注分析，不污染主对话 |
| 新功能设计文档 | **委派给 designer** | 结构化输出，独立上下文 |
| 大规模代码实现 | **委派给 coder** | 独立编译验证循环 |
| 测试套件编写 | **委派给 tester** | 独立编译执行环境 |
| 用户明确说"你来做" | **直接执行** | 尊重用户意图 |
| 需要跨角色协作 | **并行委派** | 同时启动多个 SubAgent，用 when_all 等待 |

**关于复杂度阈值的说明**：
- 用"涉及文件数 + 预估改动行数"替代 tool call 数量，因为这是 LLM 在分析用户请求时可以合理估算的指标
- 文件数和行数比 tool call 数量更能反映任务的实际规模
- 阈值可在工作流 JSON 中配置（后续扩展）

### 4.4 结果验收机制

Supervisor 在 SubAgent 完成后执行验收：

1. **完整性检查**：SubAgent 是否完成了 task 描述中的所有要求
2. **正确性检查**：结果是否符合约束条件（文件路径、格式等）
3. **质量检查**：代码是否编译通过、文档结构是否完整
4. **不满足时**：Supervisor 重新委派，附带更明确的指令和上次的不足点
5. **最多重试 2 次**：避免死循环
6. **完成后保留**：验收通过的 SubAgent 不自动析构，保留供后续轮次复用

---

## 5. SubAgent 核心设计

### 5.1 SubAgent 关键接口与实现重点

**核心职责**：Agent 的薄封装层，组合独立 DeepSeekClient + 独立 ToolRegistry + 独立线程。

**关键接口**：

| 方法 | 说明 | 实现重点 |
|------|------|---------|
| `runAsync(task, context)` | 启动异步执行，返回 `std::future<SubAgentResult>` | 创建 Agent 实例 → 注入两层 system prompt → 启动线程执行 `agent->run()` → 收集结果 → `promise.set_value()` |
| `cancel()` | 取消执行 | 设置共享 cancelFlag → 终止 shell 子进程 → join 线程 |
| `getStatus()` | 获取状态快照 | 通过 `statusMutex_` 保护，返回 State + Phase + 当前 Tool + 进度 |
| `getResult()` | 获取结果（仅 Completed 状态有效） | 返回 SubAgentResult（含最终回复文本、tool_calls 次数、耗时） |
| `wait()` | 阻塞等待完成 | 内部调用 `thread_.join()` |

**内部关键成员**：

| 成员 | 说明 |
|------|------|
| `unique_ptr<DeepSeekClient> client_` | 独立 HTTP 客户端实例，使用 SubAgent 专属模型（默认 flash） |
| `ToolRegistry tools_` | 独立构建：根据角色 prompt 中的 `tools available` 白名单，从工具工厂函数注册 |
| `unique_ptr<Agent> agent_` | autoApprove=true, storage=nullptr, 共享 cancelFlag |
| `shared_ptr<atomic<bool>> cancelFlag_` | 与 Supervisor 及其他 SubAgent 共享 |
| `thread` + `promise<SubAgentResult>` | 异步执行 + 结果回传 |

**状态枚举**：`Idle → Running → Completed / Failed / Cancelled`

### 5.2 SubAgent 与 Agent 的差异

| 组件 | Supervisor (Agent) | SubAgent |
|------|-------------------|----------|
| DeepSeekClient | 引用，与 App 共享 | 独立 `unique_ptr` 实例 |
| 模型 | 用户选择（默认 pro） | flash 级别廉价模型 |
| ToolRegistry | 引用，与 App 共享 | 独立构建，按自身 prompt 的 `tools available` 白名单注册 |
| cancelFlag | `shared_ptr<atomic>` (主) | `shared_ptr<atomic>` (共享同一份) |
| Storage | 有，持久化到 SQLite | 无 (`nullptr`) |
| Approval | 有，弹 UI 对话框 | 无 (`autoApprove_ = true`) |
| todoData / todo tool | 有 | 无（不注册 todo tool） |
| UI 渲染 | 有 chatHistory bubbles | 无，只返回结果 |
| 工具白名单 | 全量 12 工具（含 delegate_task） | 按自身 prompt 的 `tools available` 行独立过滤 |
| 系统 Prompt | supervisor.md | 角色 prompt + 任务上下文 |

### 5.3 SubAgent 执行流程

```
SubAgent::runAsync(task, context)
    │
    ├─ 1. 创建 Agent 实例
    │     · DeepSeekClient: 独立 new，配置 SubAgent 专属模型 (flash)
    │     · ToolRegistry: 独立构建空注册表 → 根据角色 prompt 的 "tools available" 白名单，
    │       从工具工厂函数注册对应工具（不从 Supervisor 拷贝）
    │     · Agent: autoApprove=true, storage=nullptr, cancelFlag=shared
    │
    ├─ 2. 注入 System Prompt
    │     · Layer 1: 角色 prompt (如 coder.md，含 Hard Rules + Workflow + Available Tools)
    │     · Layer 2: 任务上下文 (Supervisor 动态生成)
    │
    ├─ 3. 启动线程
    │     thread_ = std::thread([this]() {
    │         agent_->startTurn(task);
    │         agent_->run();              // 完整 Streaming → ExecutingTools → Idle 循环
    │         // 收集结果
    │         auto msgs = agent_->getSession().getContextMessages();
    │         result_ = extractResult(msgs);
    │         promise_.set_value(result_);
    │     });
    │
    └─ 4. 返回 future
          return promise_.get_future();
```

**工具注册方式**（关键修正）：
- SubAgent **不从 Supervisor 的 ToolRegistry 值拷贝**
- 而是独立构建空 `ToolRegistry`，然后根据角色 prompt 中 `tools available:` 行列出的工具名，逐一调用工具工厂函数注册
- 工具工厂函数（如 `registerShellTool`、`registerFileTools` 等）接受 `ToolRegistry&` 和 `workspacePath`，与现有 App 中的注册方式一致
- 这确保了 SubAgent 的工具集完全由自身 prompt 定义，不依赖 Supervisor 的工具集

### 5.4 SubAgentManager 关键接口与实现重点

**核心职责**：管理所有 SubAgent 的生命周期。**生命周期属于 App 层**（非 Supervisor 内部），因此可跨对话轮次存活。

**关键接口**：

| 方法 | 说明 | 实现重点 |
|------|------|---------|
| `spawn(type, task, context)` | 创建并启动 SubAgent，返回 agent_id | 检查 activeCount() < 5 → 从 AgentRegistry 获取配置 → 创建 SubAgent → 调用 runAsync → 存入 `agents_` map。若已达上限，返回错误 |
| `reuse(agentId, task, context)` | 复用已有 SubAgent 执行新任务 | 查找 → 若 Idle/Completed → 直接在已有 agent_ 上调用 `startTurn(task)` 注入新 user message（不清空上下文）→ 启动新线程执行 `agent_->run()` → 返回新 `future`。若 Running → 返回错误 |
| `cancel(agentId)` | 取消指定 SubAgent | 查找 → cancel() → join → 从 map 移除 |
| `cancelAll()` | 取消所有 SubAgent | 遍历 map → 逐个 cancel + join → 清空 map |
| `getStatus(agentId)` | 获取指定 SubAgent 状态 | 查找 → getStatus()，若不存在返回特殊状态 |
| `getAllStatuses()` | 获取所有 SubAgent 状态快照 | 遍历 map，通过 `statusMutex_` 保护 |
| `waitFor(agentId, timeout)` | 等待指定 SubAgent 完成（阻塞，带超时） | 获取 future → `wait_for(timeout)` |
| `reap(agentId)` | 按 ID 析构指定 SubAgent | 查找 → 若 Idle/Completed/Failed/Cancelled → cancel + join + 从 map 移除。若 Running → 先 cancel 再移除 |
| `reapCompleted()` | 批量清理已完成的 SubAgent | 遍历 map，移除 State==Completed/Failed/Cancelled 且结果已被消费的条目 |
| `activeCount()` | 活跃 SubAgent 数量 | 返回 map 大小 |
| `getAgentIds()` | 获取所有 agent_id 列表 | 供 delegate_task action=list 使用 |

**并发上限**：

| 常量 | 值 | 说明 |
|------|-----|------|
| `MAX_CONCURRENT_SUBAGENTS` | 5 | 同时活跃 SubAgent 上限。spawn() 时检查，超限返回错误 |

**内部关键成员**：

| 成员 | 说明 |
|------|------|
| `shared_ptr<atomic<bool>> cancelFlag_` | 主 cancelFlag，所有 SubAgent 共享 |
| `unordered_map<string, unique_ptr<SubAgent>> agents_` | agent_id → SubAgent 映射 |
| `mutex mutex_` | 保护 agents_ 的并发访问 |
| `AgentRegistry& registry_` | 引用，用于获取 SubAgent 类型配置 |

---

## 6. SubAgent 跨轮复用机制

### 6.1 设计思路

SubAgentManager 属于 App 层（而非 Supervisor 内部），其生命周期与 App 一致，跨越多个对话轮次。默认情况下，SubAgent 完成后**不被自动杀死**，保留在 SubAgentManager 中供后续轮次查询和复用。

### 6.2 复用优先的调度流程

```
Supervisor 需要委派任务
    │
    ├─ 1. 调用 delegate_task action=list
    │     获取所有现有 SubAgent 的状态快照
    │
    ├─ 2. 遍历现有 SubAgent，判断是否可复用：
    │     ┌─ 同类型 (coder→coder, analyzer→analyzer)
    │     ├─ 状态为 Idle 或 Completed
    │     ├─ 任务描述相似（由 Supervisor 判断）
    │     └─ → 可复用：调用 delegate_task action=start reuse_id=<id>
    │         (SubAgentManager::reuse() → 注入新任务为 user message → runAsync())
    │         保留已有 Session 上下文，不清空
    │
    ├─ 3. 若无可用 SubAgent：
    │     ├─ 检查 activeCount() < MAX_CONCURRENT_SUBAGENTS (5)
    │     ├─ 若未满 → spawn() 创建新 SubAgent
    │     └─ 若已满 → 等待或 cancel 最旧的 Idle SubAgent 腾出位置
    │
    └─ 4. 任务完成后：
          SubAgent 状态变为 Completed，保留在 agents_ map 中
          结果被 Supervisor 读取后，标记为"已消费"
```

### 6.3 跨轮生命周期

```
Turn N:
  Supervisor 调用 delegate_task action=start → SubAgentManager::spawn() 或 reuse()
  SubAgent 执行完成 → State = Completed，保留在 agents_ map 中
  Supervisor 读取结果 → 整合回复用户 → 结果标记为"已消费"
  Supervisor Agent 线程退出（agentThread_ join）

Turn N+1:
  用户输入新消息
  Supervisor 启动新一轮 run()
  Supervisor 调用 delegate_task action=list → 获取现有 SubAgent 列表
  Supervisor 判断：
    ├─ 同类型 + Idle/Completed → 复用（reuse_id）
    ├─ 不同类型但 Idle → 可考虑 reap 释放资源
    ├─ 仍在 Running → 可等待结果或取消
    └─ Failed/Cancelled → 忽略或 reap
```

### 6.4 Supervisor 释放 SubAgent 的决策逻辑

Supervisor 通过 `delegate_task action=list` 获取所有现有 SubAgent 状态后，按以下三步决策是否释放（reap）或复用：

**步骤 1 — 必须释放**：
- 状态为 `Failed` 或 `Cancelled` → 直接 `action=reap`，无保留价值

**步骤 2 — 优先复用**（默认不释放）：
- 同类型 + 状态为 `Idle` 或 `Completed` → `action=start reuse_id=<id>`，复用已有实例
- 这是默认策略：只要类型匹配且可用，就不创建新实例

**步骤 3 — 按需释放**（仅在需要腾出资源时）：
- 需要创建新 SubAgent 但 `activeCount()` 已达上限 5 → 从以下候选中选一个 reap：
  1. 已完成且结果已消费的同类型 SubAgent（优先复用，不释放）
  2. 已完成且结果已消费的不同类型 SubAgent（释放优先级最高）
  3. Idle 且长时间未使用（>2次supervisor idle round）的 SubAgent
  4. 若以上都没有 → 拒绝新 spawn，返回错误告知 Supervisor

**释放决策总结**：

| SubAgent 状态 | 决策 | 条件 |
|--------------|------|------|
| Failed / Cancelled | **立即释放** | 无条件 |
| Completed，结果已消费，同类型 | **复用** | 无条件（默认不释放） |
| Completed，结果已消费，不同类型 | **保留** | 除非需要腾位置 |
| Completed，结果未消费 | **先读结果** | 读完后再判断复用或保留 |
| Idle，同类型 | **复用** | 无条件 |
| Idle，不同类型 | **保留** | 除非需要腾位置且空闲 >2次supervisor idle round |
| Running | **等待或取消** | 取决于新任务是否相关 |

**关键原则**：默认不释放，优先复用。只在必须腾出并发位置时才释放低价值 SubAgent。这避免了频繁创建/析构的开销。

### 6.5 清理策略

- **主动清理**：用户点击 Cancel → `cancelAll()` 杀死所有 SubAgent
- **按 ID 清理**：Supervisor 调用 `delegate_task action=reap agent_id=<id>` → `SubAgentManager::reap(id)` 析构指定 SubAgent
- **批量清理**：每轮对话结束后可调用 `reapCompleted()`，清理已完成且结果已被消费的 SubAgent
- **上限保护**：`activeCount()` 达到 5 时，新 spawn 被拒绝；Supervisor 需先 reap 一个 Idle/Completed SubAgent 腾出位置

### 6.6 线程安全分析

| 场景 | 风险 | 保证机制 |
|------|------|---------|
| UI 线程读取 SubAgent 状态 | SubAgent 线程正在更新状态 | `statusMutex_` 保护 getStatus()，返回快照拷贝 |
| Supervisor Agent 线程 spawn/reuse/reap | UI 线程同时 reap | `SubAgentManager::mutex_` 保护 agents_ map |
| SubAgent 线程仍在 run() | App 析构 | cancelFlag 先置 true → join → 再析构 |
| 新一轮 run() 读取已完成 SubAgent 结果 | SubAgent 已析构 | getResult() 返回的是已存储的拷贝，不依赖 SubAgent 存活 |
| reuse() 时 SubAgent 正在 run() | 状态冲突 | reuse() 检查 State，仅 Idle/Completed 可复用，Running 返回错误 |

**结论**：跨轮复用在线程安全上是可行的。核心保障是 `SubAgentManager::mutex_` 保护 map 操作，`statusMutex_` 保护状态读取，`cancelFlag_` 统一控制生命周期。

---

## 7. Cancel 统一机制

### 7.1 Cancel 架构

```
                    ┌──────────────────────────┐
                    │  cancelFlag               │
                    │  (shared_ptr<atomic<bool>>)│
                    │  唯一主实例                 │
                    └──────┬───────────────────┤
                           │                   │
              ┌────────────┼───────────────────┼──────────────┐
              │            │                   │              │
     ┌────────▼───┐  ┌────▼─────┐    ┌───────▼──────┐  ┌────▼─────┐
     │ Supervisor  │  │SubAgent A │    │ SubAgent B   │  │SubAgent C│
     │ Agent 实例  │  │ Agent 实例│    │ Agent 实例   │  │Agent 实例│
     └──────┬──────┘  └────┬─────┘    └──────┬───────┘  └────┬─────┘
            │              │                 │               │
            ▼              ▼                 ▼               ▼
     run() 循环      run() 循环        run() 循环       run() 循环
     每轮检查         每轮检查           每轮检查          每轮检查
     cancelFlag_     cancelFlag_       cancelFlag_      cancelFlag_
```

### 7.2 Cancel 传播链

```
用户点击 Cancel (UI)
    │
    ▼
Supervisor::cancel()
    │
    ├─ 1. cancelFlag_->store(true)          // 原子写，所有 Agent 可见
    │
    ├─ 2. client.cancel()                   // 中断当前 HTTP 请求
    │     └─ DeepSeekClient::cancelFlag = true
    │        └─ progressCallback 返回 1 → curl 中止传输
    │
    ├─ 3. approvalCv_.notify_one()          // 唤醒审批等待
    │
    ├─ 4. SubAgentManager::cancelAll()      // 取消所有 SubAgent
    │     └─ 对每个 SubAgent:
    │        ├─ subAgent.cancel()           // 触发其 Agent::cancel()
    │        ├─ 终止其 shell 子进程          // TerminateProcess (Windows)
    │        └─ thread_.join()              // 等待线程退出
    │
    └─ 5. 等待自身 Agent::run() 退出        // 主循环检测到 cancelFlag_
```

### 7.3 Agent 改动：cancelRequested_ → cancelFlag_

**当前** (`agent.h:142`):
```cpp
std::atomic<bool> cancelRequested_{false};
```

**改为**:
```cpp
std::shared_ptr<std::atomic<bool>> cancelFlag_;

// 构造时：
// - 若传入 shared_ptr → 共享
// - 若传入 nullptr → 内部自建 (兼容现有用法)
```

**`run()` 中所有 `cancelRequested_.load()` 替换为 `cancelFlag_->load()`**。

### 7.4 Shell 子进程终止

SubAgent 在执行 `exec_shell` 时可能启动子进程。Cancel 时需要终止这些进程：

**策略**：
- `shell_tool.cpp` 中 `CreateProcess` 时记录 `PROCESS_INFORMATION.hProcess`
- SubAgent 析构或 cancel 时，遍历已记录的进程句柄，调用 `TerminateProcess`
- 使用 Job Object 将 SubAgent 所有子进程绑定，Cancel 时 `TerminateJobObject` 一键终止

### 7.5 线程安全与生命周期

| 场景 | 保证机制 |
|------|---------|
| 多 Agent 同时读 cancelFlag | `atomic<bool>`，无锁安全 |
| UI 写 cancelFlag | `atomic<bool>::store()`，单写者 |
| SubAgent 析构时 Agent 仍在 run() | cancelFlag 先置 true → join thread → 再析构 |
| Supervisor 析构时 SubAgent 未完成 | 析构函数中 cancelAll() + join all |
| cancelFlag 生命周期 | shared_ptr，最后一个持有者析构时释放 |

**析构顺序约束**：
```
Supervisor::~Supervisor()
    1. cancelFlag_->store(true)       // 通知所有
    2. subAgentManager_.cancelAll()   // 取消 + join 所有 SubAgent
    3. agentThread_.join()            // 等待自身 Agent 线程
    4. client_ / agent_ 自动析构      // 安全
```

---

## 8. Agent 注册与扩展机制

### 8.1 AgentRegistry 关键接口与实现重点

**核心职责**：管理可用 SubAgent 类型的注册信息。从 `projv_workflows/` 目录下的 JSON 文件加载配置，关联 prompt 文件与工具白名单。

**关键接口**：

| 方法 | 说明 | 实现重点 |
|------|------|---------|
| `loadWorkflow(jsonPath)` | 从指定 JSON 文件加载工作流配置 | 解析 JSON → 对每个 subagent 读取 prompt 文件 → 解析 tools available → 构建 AgentTypeInfo → 替换当前注册表。失败时保留当前配置 |
| `getUserVisibleTypes()` | 获取当前工作流中所有 visible=true 的类型 | 过滤后返回，供 Supervisor prompt 中的 Available sub-agent types 列表使用 |
| `findType(name)` | 根据名称查找类型 | 返回 optional，包含 prompt 内容、工具白名单、模型配置 |
| `getMainAgentConfig()` | 获取当前工作流的 main_agent 配置 | 返回 prompt_file 和 model |
| `generatePresetWorkflow(dirPath)` | 在 projv_workflows/ 目录生成预设 coding.json | 目录不存在时创建目录 → 使用内置默认值写入 JSON 文件 |

**AgentTypeInfo 结构**：

| 字段 | 说明 |
|------|------|
| `name` | 唯一标识符（"coder", "designer", ...） |
| `displayName` | UI 显示名称 |
| `promptFile` | prompt 文件名（位于 projv_prompts/） |
| `promptContent` | prompt 文件内容（加载后缓存） |
| `description` | 简短描述 |
| `allowedTools` | 工具白名单（从 prompt 的 "tools available:" 行解析） |
| `model` | 使用的模型（默认 flash） |
| `visible` | 是否用户可见（false = 内部使用） |

### 8.2 工具白名单过滤

SubAgent 创建时，**独立构建** ToolRegistry，根据自身 prompt 中的 `tools available:` 行注册工具：

```
// SubAgent 独立构建 ToolRegistry，不从 Supervisor 拷贝
ToolRegistry tools;                              // 空注册表
auto allowedTools = agentTypeInfo.allowedTools;  // 从 prompt 解析的白名单
for (auto& toolName : allowedTools) {
    registerToolByName(tools, toolName, workspacePath);  // 调用工具工厂函数
}
// tools 只包含白名单工具
// delegate_task 和 update_todo 不在任何 SubAgent 的白名单中
```

**实现重点**：
- 需要一个 `registerToolByName(ToolRegistry&, const string& name, const string& workspacePath)` 工具工厂函数
- 该函数内部根据 name 分发到现有的 `registerShellTool`、`registerFileTools`、`registerMdFileTool` 等
- SubAgent 的工具集完全由自身 prompt 定义，与 Supervisor 的工具集解耦
- 不包含 `delegate_task`（该工具仅 Supervisor 可用）
- 不包含 `update_todo`（SubAgent 不注册 todo tool）

### 8.3 UI 下拉菜单变更

**原有设计**：UI 扫描 `projv_prompts/` 目录下所有 `.md` 文件构建下拉菜单，用户选择不同角色 prompt。

**新设计**：UI 扫描 `projv_workflows/` 目录下所有 `.json` 文件构建下拉菜单，用户选择不同工作流方案：

```
App::initialize()
    │
    ├─ 扫描 projv_workflows/ 目录 → 获取所有 .json 文件名
    ├─ 构建 workflowFiles_ 列表（去掉扩展名作为显示名，如 "coding", "review", "coding_simple"）
    ├─ 从 config.toml 读取上次使用的工作流 → 设为默认选中
    └─ 若无记录 → 选中第一个
```

用户在下拉菜单中切换工作流时：
- 调用 `loadWorkflow(jsonPath)` → 重新解析 JSON → 重建 AgentRegistry
- 调用 `replaceSystemPrompt()` 替换 Supervisor 的 system prompt 为 `main_agent.prompt_file` 的内容
- 对话历史保留，仅 system prompt 和可用 SubAgent 类型变更
- 将当前选中的工作流文件名写入 `config.toml`，下次启动时恢复

**交互变化**：
- 旧：下拉菜单选角色 prompt（coder/designer/analyzer/tester/supervisor）→ 替换单个 Agent 的角色
- 新：下拉菜单选工作流方案（coding/review/coding_simple）→ 替换整套 Supervisor + SubAgent 配置

---

## 9. UI 监控面板

### 9.1 SubAgentPanel 设计

在 UI 右侧或底部增加 SubAgent 监控面板，显示所有活跃 SubAgent 的状态。

**位置**：与 TODO Panel 并列，通过 View 菜单切换显示。

**布局**：

```
┌─────────────────────────────────┐
│ Sub-Agents           [5 max] [×]│
├─────────────────────────────────┤
│ [>] Coder          Running  45s │
│   Task: Implement UserAuth...   │
│   Phase: ExecutingTools         │
│   Tool: write_file (3/7)        │
├─────────────────────────────────┤
│ [>] Analyzer       Running  12s │
│   Task: Trace data flow in...   │
│   Phase: Streaming              │
├─────────────────────────────────┤
│ [OK] Designer      Done     2m  │
│   Result: 1 file created        │
├─────────────────────────────────┤
│ [XX] Tester        Failed   30s │
│   Error: Compilation failed     │
├─────────────────────────────────┤
│ [!!] Coder         Cancelled 5s │
│   Cancelled                     │
└─────────────────────────────────┘
```

### 9.2 状态指示

使用纯 ASCII 字符 + 颜色区分状态：

| 状态 | ASCII 标记 | 颜色 | 说明 |
|------|-----------|------|------|
| Running | `[>]` | 绿色 (0,255,0) | 正在执行，标记闪烁 |
| Completed | `[OK]` | 绿色 (0,200,0) | 成功完成 |
| Failed | `[XX]` | 红色 (255,0,0) | 执行失败 |
| Cancelled | `[!!]` | 黄色 (255,200,0) | 被取消 |
| Idle | `[--]` | 灰色 (128,128,128) | 等待中 |

以上颜色为参考，需按照当时theme处理配色

### 9.3 数据流

```
SubAgentManager::getAllStatuses()
    │
    ▼ (每 500ms 调用一次，通过 statusMutex_ 保护)
App::renderSubAgentPanel()
    │
    ├─ 遍历所有 SubAgent 状态
    ├─ 渲染 ASCII 状态标记 + 颜色 + 类型 + 任务摘要
    ├─ 渲染进度（Phase + 当前 Tool）
    ├─ 渲染 Cancel/Dismiss 按钮
    └─ 清理已 Dismiss 的完成项
```

### 9.4 与 Supervisor 的交互

- Supervisor 通过 `delegate_task` tool 创建 SubAgent → SubAgentPanel 自动显示
- 用户在 SubAgentPanel 点击 Cancel → 调用 `SubAgentManager::cancel(agentId)` → 不影响 Supervisor 和其他 SubAgent
- Supervisor 的 Cancel 按钮 → 取消所有 SubAgent + Supervisor 自身
- 新一轮对话开始时，SubAgentPanel 显示上一轮遗留的 SubAgent 状态
- 面板标题栏显示当前活跃数 / 最大上限（如 `[3/5]`）

---

## 10. 改动清单

| 文件 | 类型 | 改动内容 | 预估行数 |
|------|------|---------|---------|
| `src/core/agent.h` | 修改 | `cancelRequested_` → `shared_ptr<atomic<bool>> cancelFlag_`；构造新增 `cancelFlag` 参数（默认 nullptr 时自建）；新增 `autoApprove_` 成员 + setter | ~18 |
| `src/core/agent.cpp` | 修改 | `run()` 中 approval 分支加 `!autoApprove_` 判断；所有 `cancelRequested_.load()` → `cancelFlag_->load()`（含 `run()`、`compactSession()`、`doCompaction()`、`cancel()`、`newTurn()`） | ~15 |
| `src/core/supervisor.h` | **新增** | Supervisor 类声明 | ~50 |
| `src/core/supervisor.cpp` | **新增** | Supervisor 实现（调度逻辑） | ~150 |
| `src/core/sub_agent.h` | **新增** | SubAgent 类声明 | ~55 |
| `src/core/sub_agent.cpp` | **新增** | SubAgent 实现（独立构建 ToolRegistry） | ~130 |
| `src/core/sub_agent_manager.h` | **新增** | SubAgentManager 类声明（含 reuse/reap/并发上限） | ~60 |
| `src/core/sub_agent_manager.cpp` | **新增** | SubAgentManager 实现（含跨轮复用逻辑） | ~140 |
| `src/core/agent_registry.h` | **新增** | AgentRegistry 类声明 | ~30 |
| `src/core/agent_registry.cpp` | **新增** | AgentRegistry 实现（JSON 解析 + 预设生成） | ~110 |
| `src/core/prompts_loader.cpp` | 修改 | 新增 `scanWorkflows()` 和 `loadWorkflow()` 函数（JSON 解析 + 预设生成 fallback）；保留 presets[] 作为预设生成的默认值 | ~70 |
| `src/core/prompts.h` | 修改 | 新增配置结构体声明 + `scanWorkflows()` / `loadWorkflow()` 声明 | ~15 |
| `src/tools/delegate_tool.h` | **新增** | delegate_task 工具声明 | ~15 |
| `src/tools/delegate_tool.cpp` | **新增** | delegate_task 工具实现（含 reuse_id 支持） | ~90 |
| `src/tools/registry.h` | 修改 | 新增 `registerToolByName()` 工具工厂函数声明 | ~5 |
| `src/tools/registry.cpp` | 修改 | 实现 `registerToolByName()`：根据工具名分发到各注册函数 | ~20 |
| `src/ui/app.h` | 修改 | `Agent* agent` → `Supervisor* supervisor`；新增 `SubAgentManager` 成员；新增 SubAgentPanel 相关成员；UI 下拉菜单改为扫描 projv_workflows/ 目录 | ~20 |
| `src/ui/app.cpp` | 修改 | 初始化 Supervisor 替代 Agent；扫描 projv_workflows/ 目录；初始化 SubAgentManager（App 层生命周期）；`setupTools()` 中新增 `registerDelegateTask(tools, subAgentManager)`；UI 下拉菜单数据源变更为工作流 JSON 列表；新增 SubAgentPanel 渲染调用；修改 cancel 逻辑 | ~55 |
| `src/ui/render_subagents.h` | **新增** | SubAgentPanel 渲染声明 | ~15 |
| `src/ui/render_subagents.cpp` | **新增** | SubAgentPanel 渲染实现 | ~120 |
| `projv_workflows/coding.json` | **新增** | 预设工作流配置文件（main_agent + subagent_list），目录为空时自动生成 | ~50 |
| `projv_prompts/supervisor.md` | 修改 | Workflow 改为 Analyze → Design → Implement → Test 四阶段；tools available 新增 delegate_task；委派阈值改为文件数+行数 | ~40 |
| **合计** | — | **新增 13 个文件，修改 9 个文件** | **新增 ~1020 行，修改 ~260 行** |

---

## 11. 风险与注意事项

### 11.0 两可能致命细节
- 风险点 1 — registerTodoTool 自动注册：确实存在。agent.cpp:59 构造中无条件调用 registerTodoTool(tools, &todoData, &todoMutex)，SubAgent 创建 Agent 时也会把 todo tool 注册进去，与 5.2 节"SubAgent 无 todo tool"矛盾。需要 Agent 构造加条件判断（如 if (!isSubAgent) 或由调用方控制）。
- 风险点 2 — SubAgentManager 缺少 workspacePath：确实存在。5.4 节内部关键成员表没有 workspacePath，但 registerToolByName(tools, name, workspacePath) 需要它。需补上。

### 11.1 ToolExecutor 闭包捕获

`registerShellTool`、`registerFileTools` 等注册时，executor lambda 捕获的 `workspacePath` 等外部变量是独立的，无问题。

`registerTodoTool(tools, &todoData, &todoMutex)` 传入的是指针。**SubAgent 不注册 todo tool，不受影响。**

### 11.2 libcurl 全局状态

`curl_global_init()` 有 `g_curlInitialized` 保护只执行一次。多个 `DeepSeekClient` 实例安全。

### 11.3 线程安全

- `Agent::getStatus()` 有 `snapshotMutex_` 保护
- `Session` 有 `mtx` 保护
- `ToolRegistry` 独立实例 → 无竞争
- `DeepSeekClient` 独立实例 → 无竞争
- `SubAgentManager::agents_` 有 `mutex_` 保护
- `cancelFlag_` 是 `shared_ptr<atomic<bool>>` → 无锁安全
- SubAgent 跨轮复用：状态读取通过 `statusMutex_` 快照拷贝，map 操作通过 `SubAgentManager::mutex_` 保护
- `reuse()` 检查 State，仅 Idle/Completed 可复用，Running 返回错误

### 11.4 生命周期

SubAgent 析构顺序：
1. `cancelFlag_->store(true)` → Agent::run() 退出循环
2. 终止 shell 子进程（如有）
3. `thread_.join()` → 等待线程结束
4. `client_`、`agent_` 自动析构

**约束**：`cancelFlag` 是 shared_ptr，Supervisor 析构前必须确保所有 SubAgent 已析构，或 cancelFlag 由外部持有，生命周期长于所有 Agent。

### 11.5 并行执行注意事项

多个 SubAgent 并行时：
- 每个 SubAgent 有独立的 `DeepSeekClient` 和 `ToolRegistry`，无共享状态
- 文件系统操作可能冲突（两个 SubAgent 同时写同一文件）。**缓解策略**：
  1. Supervisor 在分配任务时确保不同 SubAgent 操作的文件集合不重叠
  2. SubAgent 的 `write_file` / `edit_file` 使用临时文件 + 原子 rename，减少写入冲突窗口
  3. 若冲突仍发生，后写入者会覆盖前者的结果，Supervisor 验收时会发现不一致并重新委派
  4. 风险等级：低。实际场景中两个 SubAgent 同时写同一文件的概率很小，且可通过上述策略兜底
- 并发上限 5 个，防止资源耗尽

### 11.6 上下文窗口

每个 SubAgent 有独立的 Session 和上下文窗口，不会污染 Supervisor 的上下文。SubAgent 的结果文本会注入 Supervisor 的对话历史，需控制长度：
- **通过 prompt 设计实现短回复**：各 SubAgent 的角色 prompt 中已包含 "Keep responses concise" 的 Hard Rule，LLM 会自然产出简洁结果
- 复杂输出（如完整设计文档、测试报告）应由 SubAgent 写入 `doc/` 目录下的 `.md` 报告文件，Supervisor 通过 `read_file` 获取完整内容
- 不对 `delegate_task` 返回结果做代码层截断——短回复由 prompt 约束自然实现

### 11.7 跨轮复用的边界条件

| 场景 | 处理方式 |
|------|---------|
| SubAgent 上一轮 Running，新一轮用户问无关问题 | Supervisor 可选择 cancel 或忽略（让其继续后台运行） |
| SubAgent 已完成但结果未被消费 | 新一轮 Supervisor 通过 action=list 发现，读取结果 |
| 需要同类型 SubAgent 但所有实例都在 Running | 等待最旧的完成，或 cancel 一个后 reuse |
| agents_ map 达到 5 上限 | 新 spawn 被拒绝；Supervisor 需先 reap 一个 Idle/Completed |
| App 退出时仍有 Running SubAgent | 析构函数中 cancelAll() + join all |

---

## 12. 扩展方向（后续）

| 方向 | 说明 | 优先级 |
|------|------|--------|
| 超时机制 | `future.wait_for(timeout)` 超时后自动 cancel | 高 |
| 复杂度阈值可配置 | 在工作流 JSON 中配置委派阈值（文件数、行数） | 中 |
| 结果流式回传 | SubAgent 通过回调实时推送 streaming text 给 Supervisor UI | 低 |
| 资源池 | 预创建 SubAgent 池，复用 DeepSeekClient 连接 | 低 |
| SubAgent 间通信 | SubAgent A 的输出直接作为 SubAgent B 的输入 | 低 |
| 持久化 SubAgent 结果 | SubAgent 结果写入 Storage，支持历史回溯 | 中 |
| 热加载配置 | `/reload-config` 命令，无需重启即可更新 coding.json | 中 |

