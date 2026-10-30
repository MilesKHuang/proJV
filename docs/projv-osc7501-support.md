# proJV 支持 OSC 7501（Program Status Protocol）

> **规范（必读，实现时按某一版固定）**：<https://www.superlogical.com/rex/docs/build/program-status>
> 名称：Program Status Protocol，OSC 命令号 **7501**，2026-10-06 发布，几天内迭代到 rev 0.4。
> 一句话：让 proJV 把"我在干活 / 我在等你审批 / 我出错了"上报给**宿主终端**，由终端决定怎么显示（标签、状态点、侧栏、通知）。
> 状态：设计稿，待实现（预计半天，约 200–300 行含测试）。

---

## 0. 速览

| 项 | 内容 |
|---|---|
| proJV 的角色 | **发送方（sender）**，不是终端；宿主（Windows Terminal / Intelligent Terminal / Ghostty / cmux / Ostlr）是接收方 |
| 改动范围 | 新增一个模块 + 5 处调用点 + 1 个配置键；**不动引擎、不动 UI 布局** |
| 成本 | 半天（含单测与 e2e 断言） |
| 风险 | 极低——不认这条 OSC 的终端会**静默忽略**；唯一必须处理的是"输出被重定向到文件时不要污染" |
| 回滚 | 配置项一键关闭；关闭后行为与今天**字节级一致** |

**它是"可选增强"，不是功能依赖**：没有宿主支持时什么都不会发生。

---

## 1. 为什么做

1. **用户侧**：proJV 跑在别人的终端里时，外层能看到它的状态——尤其是 `blocked`（等审批）时，不会被埋在另一个窗口里。
2. **生态侧**：这是 2026-10 起 CLI 侧的事实标准。已上线的发送方包括 Claude Code 2.1.295、Pi 1.1.0、Crush v0.98.0、Factory Droid v0.236.0；解析侧已进 libghostty-vt。
3. **产品侧**：Ostlr（多窗口管理器，见 `docs/ostlr-implmentation.md`）把"拥有 PTY 时收 OSC 7501"当作状态主来源（L1）。proJV 支持它，就等于在 Ostlr 里天然可见。

> 注意：**它不改变 proJV 的任何交互**。没有宿主支持时，用户完全感知不到。

---

## 2. 规范要点（实现只需要这些）

### 2.1 报文形状

```
ESC ] 7501 ; key=value[:key=value...] ST
```
- `ESC ]` = OSC 起始，`ST` = `ESC \`。
- 字段之间用 **`:`** 分隔，键值用 `=`。

### 2.2 字段

| 字段 | 必填 | 取值 / 说明 |
|---|---|---|
| `state` | ✅ | `idle` / `working` / `blocked` / `done` / `error` / `clear` |
| `kind` | 仅 blocked | `permission` / `question` / `auth` |
| `msg` | 否 | 人读的一句话，**standard base64**；控制字符先替换为空格 |
| `title` | 否 | 同上编码规则 |
| `app` | 否 | 应用名，我们固定 `projv` |
| `id` | 否 | 记录 id，我们固定 `main`（将来若要报子代理，用 `projv/skill/steve` 这类层级） |
| `progress` | 否 | 整数百分比，仅 `working` 有意义 |

### 2.3 上限（**必须截断，否则整条被丢弃**）

| 项 | 上限 |
|---|---|
| 单条序列（OSC 到 ST 全部字节） | **4096 B** |
| key 名 | 16 B |
| `msg` | 解码后 2048 B，编码后 2732 B |
| `title` | 解码后 192 B，编码后 256 B |
| `app` | 32 B |
| `id` | 128 B（每段 32 B，层级深度 8） |

### 2.4 语义

- 每条报文**替换**同 `id` 的旧记录（不是追加）；终端保留的记录数上限 64–256。
- `state=clear`：删除记录；不带 `id` = 清空全部。
- **允许不经探测直接上报**（Pi 的实现文档明确如此）。有的程序会先发 `ESC ] 7501 ; ? ST` 探测终端是否支持，但我们作为发送方可以不等——不支持的终端会丢弃。

### 2.5 安全的失败方式

不认识的终端**忽略整条序列**，不会显示乱码——这也是可以放心默认开启的原因。

---

## 3. proJV 的状态映射

proJV 的主循环状态机在 `src/core/agent.h:16`：`AgentPhase { Idle, Streaming, ExecutingTools, AwaitApproval, Error }`。

| proJV 事件 | OSC 报文 | 说明 |
|---|---|---|
| 开始处理一条用户消息（进 Streaming） | `state=working:progress=-:msg="thinking"` | |
| 正在执行工具（ExecutingTools） | `state=working:msg="tool: <name>"` | 工具名有帮助，方便外层显示"在跑 read_file" |
| 等待工具审批（AwaitApproval） | `state=blocked:kind=permission:msg="<命令摘要>"` | **最重要的一条**：这才是"需要人" |
| 一轮结束、空闲 | `state=done:msg="turn finished"`，随后 `state=idle` | `done` 让外层能闪一下；2 秒后转 idle 可选 |
| 出错（Error） | `state=error:msg="<首行错误>"` | msg 只放首行，别塞栈 |
| 技能运行中（`TuiApp::runSkillBlocking`） | `state=working:msg="skill: <name>"` | skill 期间主循环是 Idle，必须单独上报 |
| 进程退出 / 新会话 | `state=clear:id=main` | 避免宿主留一个僵尸状态 |

**v1 不做**：子记录（子代理/worker 的独立状态）。将来若做，用 `id` 层级表达，不改协议。

---

## 4. 实现设计（C++）

### 4.1 新模块 `src/core/terminal_status.{h,cpp}`

```cpp
namespace terminal_status {

// 配置开关（默认 true）。返回 false 表示当前不可用（非 TTY / 被禁用）。
bool init(bool enabled);
void shutdown();                       // 发 clear + 复位

// 状态设置：只记录"最新状态"，真正的写由 flushIfDirty() 在 UI 线程执行。
void working(const std::string& msg = "", int progress = -1);
void blocked(const std::string& kind, const std::string& msg);
void idle(const std::string& msg = "");
void done(const std::string& msg = "");
void error(const std::string& msg);
void clear();

// 脏标记 + 写入（UI 线程调用，每帧或每次 tick）
void flushIfDirty();

// 纯函数，供单测
std::string encode(const std::string& state, const std::string& kind, const std::string& msg,
                   const std::string& app, const std::string& id, int progress);

} // namespace terminal_status
```

### 4.2 三条实现纪律

1. **TTY gate**：`isatty(fileno(stdout))`（POSIX）/ `_isatty(_fileno(stdout))`（Windows）。非 TTY 时全部 no-op，并在第一次记一行 `debugLog`。→ 保证"重定向到文件/管道时 0 字节污染"。
2. **不进 std::cout**：POSIX 用 `write(STDOUT_FILENO, ...)`；Windows 用 `WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), ...)`。理由：绕开 CRT 转换与流缓冲，避免和 FTXUI 的输出交错。
3. **去重 + 截断**：同一状态连续设置只发一次；`msg` 先按 UTF-8 边界截到 2048 B，再 base64；拼好的序列超过 4096 B 就整条丢弃并 `debugLog`（宁可不上报，也不要发一条会被终端丢掉/解析错的东西）。

### 4.3 与 FTXUI 的协作（关键）

OSC 是"零宽"序列，终端消费、不占格子，在 alternate screen 下同样有效。**但 FTXUI 也在写 stdout**，两个线程同时写会交错。

所以采用**脏标记 + UI 线程 flush**：

- `Agent` 线程只调用 `terminal_status::working()/blocked()/...` 记录"最新状态"；
- `TuiApp` 在已有的 `onStreamingTick` / 渲染 tick 里调用一次 `terminal_status::flushIfDirty()`，由 UI 线程完成实际写入。

这样不需要新增线程、不需要加锁，也不会与 FTXUI 抢写。

### 4.4 调用点（5 处，改动很小）

| 位置 | 改动 |
|---|---|
| `src/core/agent.cpp` 的 phase 切换处（`Streaming` / `ExecutingTools` / `AwaitApproval` / `Error` / 回到 `Idle`） | 通过**新回调** `onPhaseChanged(AgentPhase, msg)` 通知外层；core 不依赖 TUI |
| `src/core/agent.h` | 加 `setPhaseCallback(std::function<void(AgentPhase, const std::string&)>)` |
| `src/tui/app_tui.cpp` `runSkillBlocking()` | 进入时 `working("skill: <name>")`，退出时 `idle()` |
| `src/tui/app_tui.cpp` 初始化 / 关闭 | `init(cfg.oscStatus)` / `shutdown()` |
| `src/tui/main_tui.cpp` | 进程退出路径兜底调用 `shutdown()`（含异常退出） |

### 4.5 配置

`projv_files/config.toml` 新增（默认开）：

```toml
osc_status = true    # 向宿主终端上报工作状态（OSC 7501）
```

环境变量兜底：`PROJV_NO_OSC=1` 强制关闭（用于排查"是不是它把我的输出搞乱了"）。`saveConfig` 写回时保留该键，`saveApiKey` 路径补默认值。

---

## 5. 报文样例

`ST` 实际是两个字节 `ESC \`；`msg` 由程序 base64 生成，下面括号里是对应明文。

```
# 开始思考
ESC ] 7501 ; state=working : app=projv : id=main : msg=dGhpbmtpbmc= ST        （"thinking"）

# 正在执行工具
ESC ] 7501 ; state=working : app=projv : id=main : msg=cmVhZF9maWxl ST         （"read_file"）

# 等待审批 —— 最重要的一条
ESC ] 7501 ; state=blocked : kind=permission : app=projv : id=main : msg=cGVybWlzc2lvbg== ST   （"permission"）

# 一轮结束
ESC ] 7501 ; state=done : app=projv : id=main : msg=dHVybiBmaW5pc2hlZA== ST    （"turn finished"）

# 退出 / 新会话
ESC ] 7501 ; state=clear : id=main ST
```

---

## 6. 测试

| 类型 | 用例 | 断言 |
|---|---|---|
| 单测（纯函数） | `encode()` 字段顺序与转义 | 输出以 `ESC ] 7501 ;` 开头、以 `ESC \` 结尾；字段用 `:` 分隔 |
| 单测 | base64 与控制字符 | `msg` 中含换行/制表 → 替换为空格后再编码 |
| 单测 | 截断边界 | 2048 B 明文截断不切断 UTF-8；编码后 > 2732 B 时再截；整条 > 4096 B 时**整条丢弃** |
| 单测 | 非法入参 | 未知 `state` 被拒绝（`debugLog`，不发） |
| 单测 | 去重 | 连续两次相同 `working` 只写一次；`working → blocked → working` 写三次 |
| 单测 | TTY gate | 把 stdout 重定向到文件 → 断言写入 **0 字节** |
| 单测 | 退出清理 | `shutdown()` 发一条 `clear` |
| e2e | 用 `tests/e2e` 的 fake host 读 stdout，匹配 `]7501;` | 一轮对话后至少出现 `working` 与 `done` |
| 手工 | Windows Terminal / Intelligent Terminal / Ghostty / Ostlr 各一次 | 有显示即通过；**不显示不算失败**（宿主不支持），记录到文档 |

新增测试文件要加进 `tests/CMakeLists.txt` 的 `TEST_SOURCES`（不是 glob），见仓库 `AGENTS.md`。

---

## 7. 验收（DoD）

1. 在至少一个宿主里，状态变化可见（`working` / `blocked` / `done`）。
2. 输出重定向到文件或管道时，**0 字节污染**。
3. 单测覆盖：编码、去重、截断、TTY gate、退出清理。
4. 退出时发 `clear`，宿主不留僵尸状态。
5. `osc_status = false` 或 `PROJV_NO_OSC=1` 时，行为与今天**字节级一致**。

---

## 8. 风险与对策

| # | 风险 | 对策 |
|---|---|---|
| R1 | 与 FTXUI 抢写 stdout，画面错乱 | 脏标记 + **UI 线程 flush**（§4.3），不让 agent 线程直接写 |
| R2 | 被重定向时污染日志/管道 | TTY gate（`isatty`），非 TTY 全 no-op |
| R3 | 序列过长被宿主整条丢弃 | 按 §2.3 截断；超 4096 B 整条不发并记日志 |
| R4 | Windows 控制台编码转换把字节搞坏 | 用 `WriteFile` 二进制直写，不经 CRT |
| R5 | 某些宿主"必须探测才显示" | 我们按规范允许的方式**直接上报**；宿主严格与否不影响我们的正确性（记录，不兜底） |
| R6 | 用户觉得"输出里有奇怪字符" | `PROJV_NO_OSC=1` 一键关闭；文档写明这是零宽序列 |

---

## 附录 A · 参考与链接

| 类型 | 链接 | 用途 |
|---|---|---|
| **规范（权威）** | <https://www.superlogical.com/rex/docs/build/program-status> | 唯一来源。实现时在注释里写清按哪一版（rev 0.2 / 0.3 / 0.4，几天内迭代过三次） |
| 解析侧参考 | `ghostty-org/ghostty` PR **#14560**（libghostty-vt） | 看终端如何解析与保存记录；对照我们的边界处理 |
| 终端侧参考实现（Go） | `github.com/Gaurav-Gosain/tuios` → `internal/progstatus` | 完整实现 + 全部上限常量（`MaxSequence=4096`、`MaxKey=16`、`MaxRecords=256` 等），与 §2.3 一一对应 |
| Python 参考实现 | <https://pypi.org/project/osc7501/> | 含 probe/detect、状态机、base64 规则；可用来交叉验证我们的报文 |
| 已知缺陷（了解即可） | `anthropics/claude-code` issue **#105534** | DA1 与 7501 应答竞速；对"我们作为发送方"无影响，但能解释"为什么有的宿主看起来不支持" |
| 生态状态（2026-10） | Claude Code 2.1.295、Pi 1.1.0、Crush v0.98.0、Factory Droid v0.236.0 已上报 | 判断"值不值得做"的依据 |

---

## 附录 B · 与 Ostlr（多窗口管理器）的契约

Ostlr 把 proJV 当作可纳管的 agent 之一，其状态来源 L1 就是本协议（见 `docs/ostlr-implmentation.md` §4.3）。双方约定：

1. **proJV 不等探测**：按规范允许的方式直接上报；Ostlr 若回 `OSC 7501;?`，proJV 无需改变行为。
2. **`id` 必须稳定**：固定 `main`。否则宿主会把同一进程显示成多条记录。
3. **`msg` 单行、可读、≤120 字符**：宿主侧多为单行展示（Ostlr 的会话列表、终端的标签）。不要塞多行或栈。
4. **`blocked` 必须带 `kind`**：Ostlr 用 `kind=permission` 判断"该叫醒人"。
5. **退出必须 `clear`**：否则宿主留一个僵尸状态，用户会以为 proJV 还在跑。

---

## 附录 C · 实现清单（文件级）

| 文件 | 改动 | 估时 |
|---|---|---|
| `src/core/terminal_status.h` / `.cpp`（新增） | `encode()` 纯函数、去重、TTY gate、脏标记与 flush | 3 h |
| `src/core/agent.h` | 新增 `setPhaseCallback()` | 0.2 h |
| `src/core/agent.cpp` | 5 处 phase 切换上调回调（Streaming / ExecutingTools / AwaitApproval / Error / 回 Idle） | 0.5 h |
| `src/tui/app_tui.cpp` | `runSkillBlocking()` 前后上报；`initialize`/`shutdown`；`onStreamingTick` 里 `flushIfDirty()` | 0.8 h |
| `src/tui/main_tui.cpp` | 退出路径兜底 `shutdown()` | 0.2 h |
| `src/core/config.cpp` + `src/client/models.h` | `osc_status` 键的读写与默认值 | 0.3 h |
| `tests/unit/test_osc7501.cpp` + `tests/CMakeLists.txt` | 单测（编码 / 去重 / 截断 / TTY / 清理） | 1.5 h |
| `tests/e2e` | fake host 断言 `]7501;` 出现 | 1 h |
| `docs/{en,zh}/user-guide/08-reference.md` | 配置表加一行 `osc_status`（双语同步） | 0.3 h |

合计约 **1 个工作日**（含测试）。

验收命令：

```
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
build\tests\Release\proJV_tests.exe -tc="osc*"
```
