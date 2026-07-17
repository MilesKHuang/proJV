# Sub-Agent 设计方案

> 版本: v1.0  
> 日期: 2026-07-17  
> 状态: 设计阶段

---

## 1. 背景与目标

### 1.1 现状

proJV 当前为单 Agent 架构，整个项目生命周期共 **3 个线程**：

| 线程 | 位置 | 生命周期 |
|------|------|----------|
| UI 主线程 | `main.cpp` WinMain 消息循环 | 常驻 |
| Agent 工作线程 (`agentThread_`) | `app.h:117` → `app.cpp:840` | 每次用户输入时创建，完成后回收 |
| 模型获取线程 (`fetchModelsThread_`) | `app.h:119` → `app.cpp:296` | 初始化时短暂运行 |

### 1.2 目标

在**不改动 Agent 核心状态机 (`agent.cpp` 的 `run()`)** 的前提下，抽象出一个轻量级 SubAgent，使 main agent 能派生子任务并行执行，完成后取回结果。

### 1.3 设计原则

- **Agent 核心零改动**：`run()` 状态机（Streaming → ExecutingTools → Idle 循环）保持不变，只加构造参数
- **SubAgent 是薄封装层**：组合 Agent + 独立 DeepSeekClient + 独立 ToolRegistry
- **最小依赖**：不需要 Storage、不需要 UI 渲染、不需要 Approval 交互、不需要 todo tool

---

## 2. 架构对比

### 2.1 当前 main agent 依赖图

```
App
 ├── DeepSeekClient (shared via &)
 ├── ToolRegistry  (shared via &)
 ├── Storage*       (shared via *)
 ├── Agent
 │    ├── Session (owned)
 │    ├── cancelRequested_ (owned atomic)
 │    └── todoData (owned)
 └── agentThread_ (owned thread)
```

### 2.2 SubAgent 依赖图

```
SubAgent
 ├── DeepSeekClient (owned unique_ptr)     ← 独立实例
 ├── ToolRegistry   (owned, 值拷贝)        ← 独立实例
 ├── cancelFlag     (shared_ptr<atomic>)   ← 与 main agent 共享
 ├── Agent
 │    ├── Session (owned)
 │    ├── cancelFlag_ → 外部 shared_ptr
 │    └── autoApprove_ = true             ← 跳过审批
 └── thread_ (owned)
```

## 2.3 关键差异

| 组件 | main agent | sub-agent |
|------|-----------|-----------|
| DeepSeekClient | 引用，与 App 共享 | 独立 owned 实例 |
| ToolRegistry | 引用，与 App 共享 | 值拷贝，完全独立 |
| cancelFlag | 自建 atomic | 外部 shared_ptr，与 main agent 共享 |
| Storage | 有，持久化到 SQLite | 无，Session 即上下文 |
| Approval | 有，弹 UI 对话框 | 无，auto-approve |
| todoData / todo tool | 有，注册到 ToolRegistry | 无，sub-agent 不注册 todo tool |
| UI 渲染 | 有 chatHistory bubbles | 无，只返回结果 |

---

## 3. 核心设计

### 3.1 Cancel 共享机制

main agent 和所有 sub-agent 共享同一个 `std::shared_ptr<std::atomic<bool>>`：

```
main agent cancelFlag (shared_ptr)
    ├── main agent 的 Agent::cancelFlag_
    ├── sub-agent A 的 Agent::cancelFlag_
    └── sub-agent B 的 Agent::cancelFlag_
```

main agent 调用 `cancelFlag->store(true)` → 所有 Agent 的 `run()` 循环同时退出。

Agent 构造时接受外部 cancelFlag，若传 `nullptr` 则内部自建，兼容现有 main agent 用法。`run()` 中所有 `cancelRequested_.load()` 替换为 `cancelFlag_->load()`。

### 3.2 ToolRegistry 独立实例

`ToolRegistry` 是纯值类型，`std::unordered_map` 的默认拷贝构造产生两份完全独立的 map，零共享、零锁。SubAgent 构造时从 main agent 的 ToolRegistry 值拷贝一份。

**注意**：`registerTodoTool` 注册时传入了 Agent 的 `todoData` 指针，值拷贝后 executor 仍指向原指针。SubAgent 不注册 todo tool，因此不受影响。

### 3.3 跳过 Approval

Agent 新增 `autoApprove_` 标志。`run()` 中 destructive command 检测分支增加判断：

```
if (hasD && !autoApprove_ && !destructiveApproved_.load()) {
    // 等待 approval...
}
// autoApprove_=true → 直接跳过，进入 ExecutingTools
```

SubAgent 构造 Agent 时设置 `autoApprove_ = true`，所有工具调用自动批准。

### 3.4 无 Storage / 无 UI / 无 todo tool

- **Storage**：`addPersistedMessage()` 内部已有 `if (storage && storage->isOpen())` 检查，设 `nullptr` 后只写 Session，不写 DB
- **UI**：不需要 chatHistory bubbles、不需要 render 相关逻辑
- **todo tool**：不注册，避免闭包捕获原 Agent 的 todoData 指针

---

## 4. 改动清单

### 4.1 Agent 核心 (`agent.h` / `agent.cpp`)

| 改动 | 说明 | 约行数 |
|------|------|--------|
| `cancelRequested_` → `cancelFlag_` | `std::atomic<bool>` 改为 `std::shared_ptr<std::atomic<bool>>` | 5 |
| 构造新增 `cancelFlag` 参数 | 默认 `nullptr` 时内部自建，兼容现有代码 | 8 |
| 新增 `autoApprove_` 成员 + setter | 构造参数或 `setAutoApprove()` | 5 |
| `run()` 中 approval 分支加 `!autoApprove_` 判断 | 跳过审批等待 | 2 |
| 所有 `cancelRequested_.load()` → `cancelFlag_->load()` | 全局替换 | 10 |

### 4.2 新增 SubAgent (`src/core/sub_agent.h` / `.cpp`)

| 内容 | 说明 | 约行数 |
|------|------|--------|
| 类声明 | 接口：`runAsync()`、`cancel()`、`getStatus()` | 40 |
| 构造/析构 | 初始化 client_、tools_（值拷贝）、agent_（autoApprove=true） | 30 |
| `runAsync()` | 创建 `promise/future`，启动线程 | 15 |
| `runImpl()` | 线程入口：注入 system prompt → `startTurn` → `run` → 取 Session 消息 → `promise.set_value()` | 25 |
| `cancel()` / `getStatus()` | 委托给 agent_ | 10 |

### 4.3 总计

| 模块 | 新增 | 修改 |
|------|------|------|
| `agent.h/cpp` | 0 | ~30 行 |
| `sub_agent.h`（新） | ~40 行 | — |
| `sub_agent.cpp`（新） | ~80 行 | — |
| **合计** | **~120 行** | **~30 行** |

---

## 5. 风险与注意事项

### 5.1 ToolExecutor 闭包捕获

`registerShellTool`、`registerFileTools` 等注册时，executor lambda 捕获的 `workspacePath` 等外部变量在值拷贝后是独立的，无问题。

`registerTodoTool(tools, &todoData, &todoMutex)` 传入的是指针，值拷贝后仍指向原对象。**SubAgent 不注册 todo tool，不受影响。**

### 5.2 libcurl 全局状态

`curl_global_init()` 有 `g_curlInitialized` 保护只执行一次。多个 `DeepSeekClient` 实例安全。

### 5.3 线程安全

- `Agent::getStatus()` 有 `snapshotMutex_` 保护
- `Session` 有 `mtx` 保护
- `ToolRegistry` 独立实例 → 无竞争
- `DeepSeekClient` 独立实例 → 无竞争

### 5.4 生命周期

SubAgent 析构顺序：
1. `cancelFlag_->store(true)` → Agent::run() 退出循环
2. `thread_.join()` → 等待线程结束
3. `client_`、`agent_` 自动析构

**约束**：`cancelFlag` 是 shared_ptr，main agent 析构前必须确保所有 sub-agent 已析构，或 cancelFlag 由外部持有，生命周期长于所有 Agent。

---

## 6. 扩展方向（后续）

| 方向 | 说明 |
|------|------|
| 多 sub-agent 并行 | main agent 同时派发多个 sub-agent，用 `when_all` 等待 |
| 结果流式回传 | sub-agent 通过回调实时推送 streaming text 给 main agent UI |
| 超时机制 | `future.wait_for(timeout)` 超时后 cancel |
| 资源池 | 预创建 sub-agent 池，复用 DeepSeekClient 连接 |