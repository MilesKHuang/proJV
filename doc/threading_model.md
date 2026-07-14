# proJV 线程模型

> 最后更新: 2026-07-15
> 内容: 双线程拓扑、Agent 状态机、同步机制、典型场景时序

## 1. 线程拓扑

```
+-------------------------------------------------------------+
|                      PROCESS BOUNDARY                        |
|                                                             |
|  +------------------+     curl_easy_perform()               |
|  |   AGENT THREAD   |--------[BLOCKING]------------------+  |
|  |                  |   SSE callbacks fire here          |  |
|  |  agent->run()    |<-- onText / onThinking / onFinish -+  |
|  |                  |<-- onToolCall / onUsage / onError -+  |
|  |                  |                                      |
|  |  SHARED STATE ---+--- snapshotMutex_                   |
|  |                  |    approvalMutex_ + approvalCv_     |
|  |                  |    cancelRequested_ (atomic)        |
|  |                  |    cancelFlag (atomic, in client)   |
|  |                  |                                      |
|  +--------+---------+                                      |
|           | agentThreadRunning_ (atomic)                   |
|           v                                                |
|  +------------------+                                      |
|  |    UI THREAD     |  ImGui::NewFrame -> render -> End    |
|  |    (main)        |  60fps render loop                   |
|  |                  |                                      |
|  |  render():       |                                      |
|  |   getStatus() ----> read status_ (snapshotMutex_)      |
|  |   syncChat() -----> read DB (agent->getNewMsgsSince)   |
|  |   checkAgent ----> join finished thread                 |
|  |   Cancel btn -----> agent->cancel() -> cancelFlag=true |
|  |                  |                                      |
|  |  chatHistory ----+-- UI-thread-only deque               |
|  |  totalTokens ----+-- int, written by Agent thread       |
|  |                    (non-atomic, menu display only)      |
|  +------------------+                                      |
|                                                            |
|  +------------------+                                      |
|  | StorageWriteQueue |  serialized DB writes               |
|  |  (dedicated)      |  agent->enqueueWrite(...)          |
|  +------------------+                                      |
+-------------------------------------------------------------+
```

核心认知：不存在独立的 "Streaming Thread"。`curl_easy_perform()` 在 **Agent Thread** 上同步阻塞。所有 curl 回调（write, header, progress）都在同一个 Agent Thread 上触发。这是 "Agent 独立线程" 重构的核心 -- `DeepSeekClient` 内部不再有 `workerThread`。

线程生命周期：
- Agent 线程在每轮对话中通过 `launchAgentThread()` 创建
- `agentThreadRunning_` (atomic) 在 `run()` 前设为 true，`run()` 返回后设为 false
- UI 线程的 `checkAgentThread()` 在线程结束时调用 `join()`
- `launchAgentThread()` 在创建新线程前先 `joinAgentThread()` 确保干净重启

## 2. Agent 状态机 (AgentPhase)

```
                         +----------+
        startTurn()      |          |
     ------------------->|   Idle   |<-----------------------------------+
                         |    (0)   |                                    |
                         +----+-----+                                    |
                              | run()                                    |
                              v                                          |
                         +----------+                                    |
                         |Streaming |  streamBlocking() blocking         |
                         |   (1)    |  callbacks update status_          |
                         +----+-----+                                    |
                              |                                          |
               +--------------+--------------+                           |
               |              |              |                           |
               v              v              v                           |
          HTTP 4xx?     has tool_calls?  text-only reply                 |
               |              |              |                           |
               v              |              v                           |
       +--------------+       |      addPersistedMessage                 |
       | Retry (<=3x) |       |      ------------------------------> Idle|
       | compaction   |       |                                          |
       | repairSession|       |                                          |
       +---+------+---+       |                                          |
           |      |           |                                          |
           v      v           |                                          |
         ok     fail          |                                          |
           |      |           |                                          |
           |      v           |                                          |
           |  +----------+    |                                          |
           |  |  Error   |    |                                          |
           |  |   (4)    |    |                                          |
           |  +----+-----+    |                                          |
           |       |          |                                          |
           |       +----> Idle|                                          |
           |                  |                                          |
           +---> back to Streaming                                       |
                              |                                          |
                              v                                          |
                    has destructive cmd?                                 |
                         |         |                                     |
                     YES |         | NO                                  |
                         v         |                                     |
               +--------------+    |                                     |
               |AwaitApproval |    |                                     |
               |     (3)      |    |                                     |
               | cv_.wait()   |    |                                     |
               +---+------+---+    |                                     |
                   |      |        |                                     |
             allow |      | deny   |                                     |
                   |      v        |                                     |
                   |  clearLast    |                                     |
                   |  -> Streaming (retry)                               |
                   v              |                                      |
            +--------------+      |                                      |
            |ExecutingTools|      |                                      |
            |     (2)      |      |                                      |
            | per-tool exec|      |                                      |
            +------+-------+      |                                      |
                   |              |                                      |
         +---------+---------+    |                                      |
         |         |         |    |                                      |
         v         v         v    |                                      |
       normal  consec-err  max-depth                                     |
         |     (>=2 err)   (30 rounds)                                   |
         |         |         |                                           |
         v         v         v                                           |
       -> Streaming  strip   strip                                       |
                    -> Streaming -> Streaming                            |
                                                                         |
                     +---------------------------------------------------+
                     |  (approval allow also goes here)
```

### 状态说明

| 阶段 | 含义 | UI 显示 |
|------|------|---------|
| `Idle` (0) | 等待用户输入 | 输入框可用 |
| `Streaming` (1) | `curl_easy_perform()` 阻塞中，SSE 数据到达 | 旋转指示器 + 字符计数 |
| `ExecutingTools` (2) | 在 Agent 线程上顺序执行工具调用 | "运行中: tool_name (n/m)" |
| `AwaitApproval` (3) | 阻塞在 `approvalCv_.wait()`，等待用户审批 | 弹出审批对话框 |
| `Error` (4) | 不可恢复的错误 | 聊天区显示错误消息 |

## 3. 线程交汇点

```
TIMELINE --------------------------------------------------------->

UI Thread:    [render] [render] [render] [render] [render] ...
                  |        |        |        |
                  | getStatus() every frame --- snapshotMutex_ ----+
                  |                                                |
Agent Thread: [streamBlocking --BLOCKING-->] [process] [Idle]      |
                  |                                                |
                  | callbacks fire here:                           |
                  |   onText() --> write status_.streamingText ----+
                  |   onFinish() --> write status_ + final flush --+
                  |   onError()  --> streamError_ / streamErrorMsg_
                  |
                  +-- cancelFlag (atomic) <-- UI: Cancel button
                  +-- cancelRequested_ (atomic) <-- UI: cancel()
                  |
                  +-- approvalCv_.wait() <-- UI: approveTool()
                  |    (Agent waits for user to approve destructive cmd)
                  |
                  +-- onTokenUsage() --> totalPromptTokens (int)
                        UI reads in menu bar rendering
```

### 交汇点分类

| 交汇点 | 方向 | 同步机制 | 争用风险 |
|--------|------|----------|----------|
| `status_` 快照 | Agent -> UI | `snapshotMutex_` | 中等（回调中 16:1 批量更新） |
| Cancel 信号 | UI -> Agent | `cancelFlag` / `cancelRequested_` (atomic) | 无 |
| 审批对话框 | Agent <-> UI | `approvalMutex_` + `approvalCv_` | 低（偶发） |
| Token 计数 | Agent -> UI | 无（int 直接写入） | 极低（仅影响菜单栏显示） |
| DB 写入 | Agent -> WriteQueue | `StorageWriteQueue` 串行化 | 无 |
| DB 读取 | UI -> DB | `agent->getNewMessagesSince()` | 低（大部分帧为空查询） |
| `checkAgentThread` | UI -> Agent | `agentThreadRunning_` atomic + `join()` | `join()` 可能短暂阻塞 |

### 数据所有权

| 数据 | 所属线程 | 读取者 |
|------|----------|--------|
| `currentContent_` / `currentReasoning_` | Agent（写） | Agent（读）；UI 通过 `getStatus()` 拷贝 |
| `chatHistory` (deque) | 仅 UI | 仅 UI |
| `session.messages` | Agent（通过 `addPersistedMessage`） | Agent（buildRequest）；UI 通过 DB 读取 |
| `todoData` | Agent（通过工具写入） | UI 通过 `copyTodoData()`，加 `todoMutex_` |
| `totalPromptTokens` / `totalCompletionTokens` | Agent（通过回调写入） | UI（菜单栏读取） |
| `cachedContextTokens_` | Agent（缓存，`contextTokensDirty_` 标记） | UI 通过 `getContextPressure()` |

## 4. 典型场景时序

### 4.1 正常文本回复

```
UI                      Agent                    curl / DeepSeek API
|                       |                        |
+-- startTurn() ------->|                        |
+- launchAgentThread()->| new thread             |
|                       +-- run()                |
|                       +-- Streaming            |
|                       +-- streamBlocking() --->| POST + SSE
|  [render every frame] |   [BLOCKING]           | HTTP/1.1 200
|  getStatus() -->lock  |   onText() <-----------| data: {"delta":...}
|  (lock free, returns) |   onText() <-----------| data: {"delta":...}
|                       |   ... (N iterations)   | ...
|                       |   onFinish() <---------| data: [DONE]
|                       |   [returns true]       |
|                       +-- addPersistedMessage  |
|                       +-- Idle                 |
|                       +-- agentRunning=false   |
|  getStatus() -->Idle  |   thread exit          |
|  syncChat() -->bubble |                        |
|  checkAgentThread()   |                        |
|  join() (instant)     |                        |
|  [input box enabled]  |                        |
```

### 4.2 破坏性命令触发审批

```
UI                      Agent
|                       |
|                       +-- Streaming -> has tool_calls
|                       +-- hasDestructiveCommand() -> true
|                       +-- phase_ = AwaitApproval
|                       +-- updateSnapshot()
|                       +-- cv_.wait(approvalDone_)  <-- BLOCKING
|                       |
|  getStatus()          |
|  -> AwaitingApproval  |
|  -> OpenPopup("Delete |
|     Confirmation")    |
|  -> User clicks Allow |
|  -> approveTool(1)    |
|  -> approvalDone=true |
|  -> cv_.notify_one() --> wakes up
|                       +-- continue: ExecutingTools
|                       +-- run tools one by one
|                       +-- -> Streaming (next round)
```

### 4.3 HTTP 400 重试（上下文溢出）

```
UI                      Agent                    curl / API
|                       |                        |
|                       +-- streamBlocking() --->| POST -> 400 Bad Request
|                       |   onError("HTTP 400")  |
|                       |   [returns false]      |
|                       +-- detect "HTTP 4"      |
|                       +-- repairSession()      |
|                       +-- doCompaction()       |  (仅 context-length 错误)
|                       +-- rebuild request      |
|                       +-- streamBlocking() --->| POST -> 200 OK
|  [UI always shows     |   ... continue         |
|   Cancel active]      |                        |
```

重试逻辑说明：
- Agent 层: HTTP 4xx 最多 3 次重试
- `doCompaction()` 仅在 context-length 相关错误时触发（非所有 4xx）
- `DeepSeekClient::streamBlocking` 也有透明重试，但仅针对 429 + 5xx（最多 3 次）
- 两层重试作用于不同的 HTTP 状态码范围，不会出现双重嵌套重试爆炸

### 4.4 用户取消

```
UI                      Agent                    curl / API
|                       |                        |
|  [Press Cancel]       |                        |
+-- agent->cancel()     |                        |
|  cancelFlag=true ---->|                        |
|                       | [progressCallback detects]
|                       |   <-- abort transfer --|
|                       | [retry path detects]   |
|                       |   cancelFlag=true      |
|                       |   -> skip Sleep, break |
|                       +-- cleanup, Idle        |
|  [input box restored] |                        |
```

### 4.5 多工具调用多轮交互

```
UI                      Agent                    curl / API              Shell/Files
|                       |                        |                       |
|                       +-- Streaming            |                       |
|                       +-- streamBlocking() --->| POST + SSE            |
|  getStatus() -->lock  |   onText() <-----------| data: {"delta":...}   |
|  (lock free)          |   onToolCall() <-------| tool_calls: [         |
|                       |     reads/exec_shell   |   read_file,          |
|                       |                        |   exec_shell,         |
|                       |                        |   edit_file           |
|                       |                        | ]                     |
|                       |   onFinish() <---------| data: [DONE]          |
|                       |   [returns true]       |                       |
|                       +-- addPersistedMessage  |                       |
|                       |   (assistant + 3 tool_calls)                   |
|                       |                        |                       |
|                       +-- ExecutingTools (2)   |                       |
|                       |                        |                       |
|  status_.toolProgress |                        |                       |
|  = 1/3, "read_file"   |                        |                       |
|  getStatus() -->lock  |                        |                       |
|                       +-- executeTool(#1) ---->|------- read_file() -->| file system
|                       |   (blocking, Agent thd)|                       |
|                       |   toolResults[0] = <-- |<-- file content ------|
|  status_.toolProgress |                        |                       |
|  = 2/3, "exec_shell"  |                        |                       |
|  getStatus() -->lock  |                        |                       |
|                       +-- executeTool(#2) ---->|------- exec_shell() ->| OS shell
|                       |   (blocking, Agent thd)|                       |
|                       |   toolResults[1] = <-- |<-- stdout/stderr -----|
|  status_.toolProgress |                        |                       |
|  = 3/3, "edit_file"   |                        |                       |
|  getStatus() -->lock  |                        |                       |
|                       +-- executeTool(#3) ---->|------- edit_file() -> | file system
|                       |   (blocking, Agent thd)|                       |
|                       |   toolResults[2] = <-- |<-- success -----------|
|                       |                        |                       |
|                       +-- addPersistedMessage x3 (tool results)        |
|                       +-- doCompaction()       |                       |
|                       +-- repairSession()      |                       |
|                       +-- toolCallDepth_++     |                       |
|                       +-- phase_ = Streaming   |                       |
|                       |                        |                       |
|                       +-- streamBlocking() --->| POST (with 3 results) |
|  getStatus() -->lock  |   onText() <-----------| data: {"delta":...}   |
|                       |   onFinish() <---------| data: [DONE]          |
|                       +-- addPersistedMessage  |                       |
|                       |   (assistant reply)    |                       |
|                       +-- Idle                 |                       |
|  getStatus() -->Idle  |   thread exit          |                       |
|  syncChat() -->bubble |                        |                       |
|  checkAgentThread()   |                        |                       |
|  join() (instant)     |                        |                       |
```

关键特点：
- **顺序执行**: 3 个工具在 Agent 线程上按序执行，UI 帧之间插入执行
- **无 UI 阻塞**: 每帧 `getStatus()` 通过 `snapshotMutex_` 读取 `toolProgressCurrent/total`，UI 保持 60fps
- **批量化结果**: 所有工具执行完毕后才批量 `addPersistedMessage`，然后回退到 Streaming 继续与 LLM 交互
- **工具轮次累积**: `toolCallDepth_` 递增，达到 30 轮上限时触发 `softStripLastToolCalls()` 防止无限循环
- **并发错误检测**: 连续 ≥2 个工具错误 → 自动 `stripOrphanedToolCalls()` 并注入系统提示终止工具使用

Cancel 传播链：
1. UI 调用 `agent->cancel()`
2. `cancelRequested_.store(true)` (Agent)
3. `client.cancel()` -> `cancelFlag.store(true)` (DeepSeekClient)
4. `approvalDone_ = true; approvalCv_.notify_one()` (唤醒审批等待)
5. `progressCallback` 检查 `cancelFlag` -> 返回 1 -> curl 中止传输
6. `streamWriteCallback` 检查 `cancelFlag` -> 返回 0 -> 中止
7. Agent 主循环在 5 个位置检查 `cancelRequested_`，跳转到 Idle
