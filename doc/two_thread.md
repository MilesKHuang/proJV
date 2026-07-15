# 交接手册：UI 锁死排查 + 双线程重构

> 给下一个接手 Agent：本文件是唯一入口。按「关键事实 → 处置总表 → Log 表 → 顺序 → 边界」执行即可，无需重新调查。
> 症状：Agent 最后回复渲染时 UI 线程锁死，log 停在 `[Bubble] derive:`。

---

## 0. 关键事实锚点（已验证，请勿重新怀疑）

| # | 已验证结论 | 依据 | 对你的含义 |
|---|-----------|------|-----------|
| F1 | `[Bubble] derive:` 打在函数**开头**，函数体只做 `chatHistory.push_back` | `render_chat.cpp:270` | 卡死**不在** derive，而在同帧后续的 `renderChatArea`→`renderFormattedText` |
| F2 | `streamBlocking` 是**同步**的，`cancel()` 只 set `cancelFlag`，回调在同一 curl 线程内返回非零中止 | `deepseek.cpp:228-233,236` | 无跨线程互等，**cancel 死锁方向已排除，勿再查** |
| F3 | `ToolWorkerManager` 是**死代码**，全库无实例化/无 setExecutor/无 launch | 工具执行实际走 `agent.cpp:214` 同步 `executeTool` | **可直接删除，零风险** |
| F4 | 入库是异步的：`addPersistedMessage` 走 `enqueueWrite`，UI 从独立只读连接 `readDb_` 查 | `agent.cpp:65-73`, `storage.cpp:219` | 异步写 + WAL 只读连接是"假卡死"来源（见处置总表 WriteQueue） |

**最可疑靶心 = renderFormattedText / ImGui::MarkdownEx 对最后一条长回复渲染不收敛/死循环（纯 UI 线程问题，与线程数无关）。**

---

## 1. 处置总表（第一步：双线程重构 = 降噪）

目标：常驻线程收敛为 **UI + Agent 两个**（shell reader 是临时 I/O 线程，保留）。

| 线程 | 处置 | 具体动作 | 验收判定（完成信号） | 风险 |
|------|------|---------|---------------------|------|
| **ToolWorker** | 直接删除 | 删 `tool_worker.h/.cpp`；移除构建脚本引用与所有 `#include` | 全局搜索 `tool_worker` 仅无命中；编译通过 | 无（死代码 F3） |
| **WriteQueue** | 并入 Agent 线程 | 不给 `agent->enqueueWrite` 赋值 → 自动走同步 `write()`（`agent.cpp:71-72`）；再删 `StorageWriteQueue` 类与 start/stop/flush 调用 | 编译通过 + 连续 5 轮长回复无写丢失 + 无独立写线程 + `flush()` 调用点已清理 | 低（写在 Agent 线程，UI 无感）；**需复核 WAL 见边界 C1** |
| **fetchModels** | 去 detach | `app.cpp:301` 去掉 `.detach()`，改启动同步或复用；用 `modelsLoading` 标志显示 loading | 无网/超时下启动不卡死；无游离 detach 线程 | 极低 |
| **shell reader** | **保留** | 不动 | —— | 删则管道死锁 |
| UI / Agent | 保留 | —— | —— | —— |

**执行顺序**：ToolWorker（零风险先做）→ WriteQueue（核心，消除 F4 假卡死）→ fetchModels。

---

## 2. renderFormattedText Log 表（第二步：坐实靶心）

前提：处置总表阶段做完后，**先观察卡死是否仍复现**；复现才加 Log。
目的：判定卡死是否在 markdown 渲染内部。所有 log 加在 `render_chat.cpp`。

| 埋点 | 位置 | 建议 log | 判读规则（关键） |
|------|------|---------|-----------------|
| A1 | `renderFormattedText` 进入(131) | `[Render] fmt ENTER len=%zu` | 与 A4 配对 |
| A2 | seg 循环体内(152) | `[Render] seg #%d isCode=%d size=%zu BEGIN` | 与 A3 配对 |
| A3 | seg 处理后(181/184) | `[Render] seg #%d END` | —— |
| A4 | `renderFormattedText` 退出(185) | `[Render] fmt EXIT` | —— |
| B1 | `renderChatArea` for 开头(443) | `[Render] bubble i=%d role=%s len=%zu` | 定位卡在哪条气泡 |
| B2 | tool 合并 while 内(449) | `[Render] merge i=%d` | 确认 `i` 是否推进 |

**判读结论表**：

| 观察到的最后日志 | 结论 | 下一步 |
|-----------------|------|-------|
| 停在 `seg #N BEGIN` 无对应 `END` | ✅ 坐实靶心：markdown/child 渲染不收敛 | 进入修复分支（见下） |
| 停在某 `bubble i=X` 无 `fmt ENTER` | 卡在气泡外层布局 | 查该 role 分支的 BeginChild/AutoResizeY |
| `merge i` 不再增长 | tool 合并推进异常 | 查 449 行 while 条件 |
| `chatArea` 正常结束仍卡 | 不在渲染层 | 回查方向二/三（锁竞争/WAL） |

**修复方向（坐实靶心后）**：在 `renderFormattedText` 入口检测**未闭合 ``` / 超长无换行行**，命中则降级为 `ImGui::TextUnformatted` 纯文本。**不是**直接换解析器。

---

## 3. 执行顺序与决策分叉

```
处置总表：ToolWorker 删除 → WriteQueue 并入 → fetchModels
        ↓
   观察：最后回复是否仍卡死？
        ├─ 否 → 完成（原是 F4 假卡死）
        └─ 是 → 加 Log 表 A/B → 按判读结论表定位
                    ├─ 停在 seg 内 → markdown 降级修复
                    └─ chatArea 正常结束 → 回查锁/WAL
```

---

## 4. 边界（不要做什么）

- **C1（必做复核）**：WriteQueue 并入后，WAL 下同进程读写连接仍可能撞 busy handler。缓解：把 `readDb_` busy timeout 从 3000ms 调小（如 500ms，`storage.cpp:106`），并让 UI 只读查询仅在有新消息信号时触发，而非每帧全量 `id>?`。
- shell reader **不许删**（Windows 管道缓冲填满即死锁的正确性刚需）。
- markdown 修复走"检测+降级"，**不要**贸然引入 md4c/cmark 换解析器（那是独立大工程，除非明确立项）。
- 不要在渲染路径（UI 线程）里做同步 DB 写或长阻塞操作。