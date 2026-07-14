# proJV 线程模型 & Agent 状态机

> 分析基于当前重构后代码 (streamBlocking 同步流)

## 1. 线程拓扑

```
┌─────────────────────────────────────────────────────────────┐
│                        进程边界                              │
│                                                             │
│  ┌──────────────┐     curl_easy_perform()                   │
│  │ Agent Thread │────────[阻塞]───────────────────┐         │
│  │              │  SSE callbacks fire here         │         │
│  │  agent->run()│←─ onText/onThinking/onFinish ───┤         │
│  │              │←─ onToolCall/onUsage/onError ───┘         │
│  │              │                                           │
│  │  共享状态 ───┼─── snapshotMutex_                          │
│  │              │    approvalMutex_ + approvalCv_            │
│  │              │    cancelFlag (atomic)                     │
│  │              │    cancelRequested_ (atomic)               │
│  │              │                                           │
│  └──────┬───────┘                                           │
│         │ agentThreadRunning_                               │
│         ▼                                                   │
│  ┌──────────────┐                                           │
│  │  UI Thread    │  ImGui::NewFrame → render → EndFrame     │
│  │  (main)       │  60fps 渲染循环                           │
│  │              │                                           │
│  │  render():   │                                           │
│  │   getStatus()──→ 读 status_ (snapshotMutex_)             │
│  │   syncChat()  ──→ 读 DB (lastMessageId_)                │
│  │   checkAgent  ──→ join 已完成的线程                       │
│  │   Cancel btn  ──→ agent->cancel() → cancelFlag=true     │
│  └──────────────┘                                           │
└─────────────────────────────────────────────────────────────┘
```

**关键认识**：不存在独立的 "Streaming Thread"。
`streamBlocking()` 中的 `curl_easy_perform()` 在 **Agent Thread** 上同步阻塞。
curl 的 write/progress callbacks 也在 **同一个 Agent Thread** 上触发。
这是"Agent 独立线程"重构的核心——不再需要 `DeepSeekClient` 内部的 `workerThread`。

---

## 2. Agent 状态机 (AgentPhase)

```
                        ┌──────────┐
           startTurn()  │          │
         ──────────────→│   Idle   │←─────────────────────────┐
                        │    (0)   │                          │
                        └────┬─────┘                          │
                             │ run()                          │
                             ▼                                │
                        ┌──────────┐                          │
                        │Streaming │  streamBlocking() 阻塞中  │
                        │   (1)    │  callbacks 更新 status_  │
                        └────┬─────┘                          │
                             │                                │
              ┌──────────────┼──────────────┐                 │
              ▼              ▼              ▼                 │
         HTTP 4xx?     有 tool_calls?   纯文本回复             │
              │              │              │                 │
              ▼              │              ▼                 │
     ┌──────────────┐        │      addPersistedMessage       │
     │ 重试(≤3次)    │        │      ────────────────────→ Idle│
     │ compaction   │        │                                │
     │ repairSession│        │                                │
     └──┬───────┬───┘        │                                │
        │       │            │                                │
        ▼       ▼            │                                │
     成功     失败           │                                │
        │       │            │                                │
        │       ▼            │                                │
        │  ┌──────────┐      │                                │
        │  │  Error   │      │                                │
        │  │   (4)    │      │                                │
        │  └────┬─────┘      │                                │
        │       │            │                                │
        │       └──→ Idle    │                                │
        │                    │                                │
        └──── 回到 Streaming │                                │
                             │                                │
                             ▼                                │
                    是否有破坏性命令?                          │
                       │         │                            │
                   YES │         │ NO                         │
                       ▼         │                            │
               ┌──────────────┐ │                            │
               │AwaitApproval │ │                            │
               │     (3)      │ │                            │
               │ cv_.wait()  │ │                            │
               └──┬───────┬──┘ │                            │
                  │       │    │                            │
             批准  │       │ 拒绝                           │
                  │       ▼    │                            │
                  │  clearLast │                            │
                  │  ───→ Streaming（重试）                   │
                  ▼            │                            │
           ┌──────────────┐   │                             │
           │ExecutingTools│   │                             │
           │     (2)      │   │                             │
           │ 逐个执行工具  │   │                             │
           └──────┬───────┘   │                             │
                  │           │                             │
        ┌─────────┼─────────┐ │                             │
        ▼         ▼         ▼ │                             │
   正常完成  连续错误  达到上限│                             │
        │    (≥2次)   (30次)  │                             │
        │         │         │ │                             │
        ▼         ▼         ▼ │                             │
    → Streaming  strip    strip│                             │
                 → Streaming → Streaming                    │
                                                             │
                  ┌──────────────────────────────────────────┘
                  │  (审批通过后也走这里)
```

---

## 3. 线程交汇点一览

```
时间轴 ──────────────────────────────────────────────────────→

UI Thread:   [render] [render] [render] [render] [render] ...
                  │        │        │        │
                  │ getStatus() 每帧读 status_ ──── snapshotMutex_ ────┐
                  │                                                    │
Agent Thread: [streamBlocking ─ 阻塞 ─→] [处理结果] [Idle]            │
                  │                                                     │
                  │ callbacks fire here:                                │
                  │   onText() ──→ 写 status_.streamingText ────────────┘
                  │   onFinish() ──→ 写 status_ + streamFinished_
                  │   onError()  ──→ streamError_ / streamErrorMsg_
                  │
                  ├─ cancelFlag (atomic) ←── UI: Cancel 按钮
                  ├─ cancelRequested_ (atomic) ←── UI: agent->cancel()
                  │
                  ├─ approvalCv_.wait() ←── UI: approveTool()
                  │   (Agent 等用户审批破坏性命令)
                  │
                  └─ onTokenUsage() → totalPromptTokens (非原子!)
                                     UI 渲染菜单栏时读取
```

**交汇点分类：**

| 交汇点 | 方向 | 同步机制 | 争用风险 |
|--------|------|----------|----------|
| `status_` 状态快照 | Agent→UI | `snapshotMutex_` | 高频(每token) → 已批量化16:1 |
| Cancel 信号 | UI→Agent | `cancelFlag` / `cancelRequested_` (atomic) | 无 |
| 审批对话框 | Agent↔UI | `approvalMutex_` + `approvalCv_` | 低（偶发） |
| Token 计数 | Agent→UI | 无（int 直接写） | 极小（只影响菜单显示） |
| DB 写入 | Agent→WriteQueue | `StorageWriteQueue` 串行化 | 无 |
| DB 读取 | UI→DB | `agent->getNewMessagesSince()` | 低（每帧空查询） |
| `checkAgentThread` | UI→Agent | `agentThreadRunning_` atomic + `join()` | join 可能短暂阻塞 |

---

## 4. 典型场景时序

### 4.1 正常文本回复

```
UI                      Agent                    curl/DeepSeek API
│                       │                       │
├─ startTurn() ────────→│                       │
├─ launchAgentThread()─→│ 新线程                 │
│                       ├─ run()                │
│                       ├─ Streaming            │
│                       ├─ streamBlocking() ────→│ POST + SSE
│  [每帧渲染]           │   [阻塞]               │ HTTP/1.1 200
│  getStatus()─→锁      │   onText() ←──────────│ data: {"delta":...}
│  (锁空闲则立即返回)    │   onText() ←──────────│ data: {"delta":...}
│                       │   ... (N次)            │ ...
│                       │   onFinish() ←─────────│ data: [DONE]
│                       │   [返回 true]          │
│                       ├─ addPersistedMessage   │
│                       ├─ Idle                  │
│                       ├─ agentRunning=false   │
│  getStatus()─→Idle    │  线程退出              │
│  syncChat()─→新bubble │                       │
│  checkAgentThread()   │                       │
│  join() (瞬时)        │                       │
│  [输入框恢复可用]      │                       │
```

### 4.2 破坏性工具触发审批

```
UI                      Agent
│                       │
│                       ├─ Streaming → 有 tool_calls
│                       ├─ hasDestructiveCommand() → true
│                       ├─ phase_=AwaitApproval
│                       ├─ updateSnapshot()
│                       ├─ cv_.wait(approvalDone_)  ← 阻塞
│                       │
│  getStatus()          │
│  → AwaitingApproval   │
│  → OpenPopup("Delete  │
│    Confirmation")     │
│  → 用户点击 Allow     │
│  → approveTool(1)     │
│  → approvalDone=true  │
│  → cv_.notify_one()  ──→ 唤醒
│                       ├─ 继续: ExecutingTools
│                       ├─ 逐个执行工具
│                       ├─ → Streaming (下一轮)
```

### 4.3 HTTP 400 重试

```
UI                      Agent                    curl/API
│                       │                       │
│                       ├─ streamBlocking() ────→│ POST → 400 Bad Request
│                       │   onError("HTTP 400")  │
│                       │   [返回 false]         │
│                       ├─ 检测到 "HTTP 4"       │
│                       ├─ repairSession()       │
│                       ├─ doCompaction()        │
│                       ├─ rebuild request       │
│                       ├─ streamBlocking() ────→│ POST → 200 OK
│  [UI 始终可 Cancel]   │   ... 正常继续         │
```

### 4.4 用户 Cancel

```
UI                      Agent                    curl/API
│                       │                       │
│  [按 Cancel]          │                       │
├─ agent->cancel()      │                       │
│  cancelFlag=true ────→│                       │
│                       │ [progressCallback 检测] │
│                       │   ← abort transfer ────│
│                       │ [重试路径检测]          │
│                       │   cancelFlag=true      │
│                       │   → 跳过 Sleep, break  │
│                       ├─ 清理, Idle            │
│  [输入框恢复]         │                       │
```

---

## 5. 已验证修复

| # | 问题 | 状态 | 说明 |
|---|------|------|------|
| A | `AwaitApproval` 映射到错误 `AgentState` | ✅ | `updateSnapshot()` 改用显式 switch 映射 |
| B | HTTP 400 只重试 1 次 | ✅ | Agent 层 3 次重试，每次 compaction |
| C | Cancel 后重试循环不退出 | ✅ | 4 处 `cancelFlag` 检查 |
| D | 死代码 `startStreaming`/`streamingWorker` | ✅ | 完全删除 (~230 行) |
| E | 回调每 token 持锁 → UI 饿死 | ✅ | 16:1 批量化 + onFinish 最终刷新 |
| F | `getEstimatedContextTokens` 每帧 O(n) | ✅ | 缓存化，消息变更时标记脏 |
