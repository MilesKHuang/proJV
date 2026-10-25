# proJV 技能引擎改进执行方案

> 状态：**P1 / P2 / P3a / P3b 已执行**（v0.6.0，170 用例 / 799 断言全绿）。
> 未完成：真机验收（需要 API Key 与 50+ 文件项目），以及 Linux 构建验证。

---

## 0. 目标

让 Monica 能处理 50~200 个文件的项目。流程是：supervisor 先分析项目、按模块拆批，**一次回复同时派给多个固定 worker**，worker 并发处理，全部完成后再交回 supervisor 统整。

配套补三块地基，主 agent 也能用：
- `read_file` 按行区间读取；
- 新增 `list_dir`，按层浏览目录结构；
- thinking 按技能/角色单独开关。

---

## 1. 问题

1. **派活是串行的。** supervisor 一次回复里的多个 `request_agent` 会在 `sub_agent.cpp:190-193` 的 for 循环里逐个同步执行（`skill_runner.cpp:510`）。
2. **`read_file` 只能整读。** 只有 `path` 参数（`file_tool.cpp:207`），超过 100KB 直接报错（`:120`）。
3. **无法分层浏览目录。** 没有列目录工具；`grep_files` 和 `file_search` 都是一次递归到底，没有深度参数（`file_tool.cpp:348`、`file_search_tool.cpp:63`）。`grep_files` 命中 100 条即停（`file_tool.cpp:388`），会让"无调用代码"判断失真。
4. **thinking 不可配置。** `ChatRequest` 没有 thinking 字段（`models.h:83`），`buildRequestBody`（`deepseek.cpp:162`）从不发送；API 默认开启 thinking，强制工具调用会被拒（400，`deepseek.cpp:187-191` 注释）。
5. **配置写错不报错。** `loadFromJson`（`skill_runner.cpp:30`）全部用 `j.value(key, 默认值)`，写错的键或值静默走默认。

---

## 2. 设计原则（执行时不得违反）

- **固定员工。** agent 在启动时按 `agents` 列表每人建一个实例，不做动态分身。supervisor 决定派给谁，没派到的就闲着。
- **派发只有一层。** 只有流程驱动者能派发，被派出去的 agent 不能再派发。
- **需要并发的 worker 只用 proJV 内部工具**（`read_file` / `write_file` / `edit_file` / `grep_files` / `file_search` / `list_dir`）。`exec_shell` 和 pytool 共用唯一的 `procRunner`，靠加锁串行（`process_runner_win.cpp:21`、`process_runner_linux.cpp:23`）。这是有意的设计：外部程序无法可靠接收 ESC。**不要修改 runner 逻辑。**
  - `tools` 是技能级配置，所有 agent 共用（`skill_runner.cpp:182`、`:194`）。因此有并发 worker 的技能，整体不配 `exec_shell` / pytool。
- **UI 回调只在技能主线程调用。** `onEvent` 只能在 `mergeAction`（join 之后）或 `run()` 中调用，worker 线程不得直接调用。
- **thinking 默认开启。** 不配置就和现在一样（开启），只有显式 `off` 才关闭。

---

## 3. 设计

### 3.1 P1：文件工具

**A. `read_file` 行区间**（`file_tool.cpp`）
- 新增可选参数 `start_line`、`end_line`，类型 `number`，从 1 开始，闭区间。整数提取用现有 `extractIntArg`。
- **两个都不传**：行为与现在完全一致。
- **传了任一个**：
  - 逐行读取，文件大小上限 `kRangeMaxFileSize = 2MB`；
  - 单次最多返回 `kRangeMaxLines = 1000` 行且不超过 `kRangeMaxBytes = 100KB`，先到先截；
  - 每行带行号，开头加 `[lines 120-480 of 2310]`；
  - 截断时末尾加 `[truncated, continue with start_line=481]`；
  - 区间非法（`end_line < start_line`、`start_line` 超过总行数）返回明确错误。
- 描述补一句："大文件请用 `start_line`/`end_line` 分段读取"。
- `file_requests`（`sub_agent.cpp:128`）支持 `path#L120-L480` 写法，解析后透传区间。分隔符用 `#`，避免与 Windows 盘符 `:` 冲突。

**B. 新增 `list_dir`**（`file_tool.cpp` 内注册）
- 参数：
  - `path`（string，可选，缺省为工作区根）；
  - `depth`（number，可选，默认 2，上限 6）；
  - `exclude`（string，可选，正则，匹配**文件名或目录名**的条目被排除，例如 `\.(obj|pdb|png)$`）。
- 输出：类似 Windows `tree /F` 的缩进树，目录名以 `/` 结尾；到达 `depth` 的目录后标注 `(N entries, not expanded)`，让 agent 决定是否往下看。
- 规则：
  - 路径**必须**走 `validateFilePath`（`file_tool.cpp:89`），不得照抄 `file_search` 的路径处理（它没有工作区校验）；
  - 跳过 `kSkipDirs` 和以 `.` 开头的目录（与 `grep_files` 一致）；
  - 同级条目按"目录在前、名称排序"输出；
  - 总输出上限 `kListMaxEntries = 500` 条，超出截断并提示缩小 `depth` 或 `path`；
  - `exclude` 正则非法时返回明确错误。
- 推荐用法写进描述：先 `list_dir(depth=2)` 看全貌，再对感兴趣的子目录加深。

**C. `grep_files` 调整**
- 新增可选参数 `max_results`（number，默认 100，上限 500），替代写死的 100（`file_tool.cpp:388`）。
- 结果被截断时末尾提示 `[truncated at N results]`，让 agent 知道结论不完整。
- 描述改为如实描述：只返回命中行（无上下文）、大小写不敏感；推荐先 `grep_files` 定位行号，再 `read_file` 读区间。

**D. 新增 `format_file`**（`format_tool.cpp` 新文件）

背景：05 原先让 Monica 去掉 `exec_shell`，worker 的"格式对齐"就只剩手工调空格，等于丢掉真格式化能力。并发约束的真正原因是**全局只有一个 `IProcessRunner`，`Run()` 全程持锁**（`iprocess_runner.h`），不是"禁止子进程"。`clang-format` 格式化单文件只要几十到几百毫秒，串行化代价可忽略；`exec_shell` 的问题是任意命令、可能长时间挂住、ESC 只能杀掉当前进程。所以用**窄接口的内置工具**替代 shell，而不是放开 `exec_shell`。

- 参数：
  - `path`（string，必填，走 `validateFilePath`）；
  - `mode`（string，可选，`"clang"` 默认 | `"whitespace"`）；
  - `style`（string，可选，clang 模式的 `-style=` 取值，默认 `file`）。
- `mode="clang"`：
  - 可执行文件来源：`config.toml` 新增 `clang_format_path`，空则用 `clang-format` 走 PATH；
  - 命令 `<exe> -i -style=<style> <path>`；向上查找最近的 `.clang-format`，找不到时用 `-style=file -fallback-style=LLVM`；
  - 单独短超时（`timeoutMs = 30000`，`kProcessTimeoutMs` 的例外），跑完比对前后内容，返回"改了几行 / 无变化"；
  - 二进制不存在时返回明确错误，提示装 clang-format 或改用 `mode="whitespace"`，**不静默降级**。
- `mode="whitespace"`：纯内置规则，零外部依赖、真并发安全。只做安全变换：行首制表符按 4 空格展开、去行尾空白、文件末尾保证一个换行、保留原换行风格（CRLF/LF）。**不重排缩进、不改换行折行**（避免制造无意义 diff）。
- 与并发原则的关系：走 runner 串行，但单文件耗时可忽略；worker 绝大部分时间在等 API，不会被拖慢。`tools` 是技能级配置，所以 supervisor 也会看到这个工具（幂等操作，无害）。
- `P3b` 里 Monica 的 `tools` 由"去掉 `exec_shell`"改为"去掉 `exec_shell`、加上 `format_file`"，`worker.md` 的"格式对齐"改为调用该工具。

### 3.2 P2：thinking 可配置 + 配置校验

**thinking**
1. `ChatRequest` 增加 `bool thinking = true`。在 `buildRequestBody`（`deepseek.cpp:162`）中：
   - `true`：**不发送**该字段（API 默认开启，与现状一致）；
   - `false`：按官方格式发送关闭字段，格式见 <https://api-docs.deepseek.com/zh-cn/guides/thinking_mode>，插在 `tool_choice` 处理（`:187`）之前。
2. 配置两级，角色级覆盖技能级：
   - 技能级：`config.json` 顶层 `thinking`；
   - 角色级：`agents[i].thinking`；
   - 取值 `"on" | "off"`，缺省 `on`。
   - 数据流：`SkillConfig` / `SkillAgent` 增加字段 → `SubAgent::configure` 增加参数 → `doRequest` 写入 `req.thinking`。
3. `force == true` 时，`doRequest` 一律 `thinking = false`（强制工具与 thinking 不兼容）。现有 `tool_choice` 兜底重试保留。
4. **bigbang 保留思考链**：`skills/bigbang/config.json` 显式加 `"tool_mode": "auto"`，使其不走 force，thinking 保持开启，同时去掉每轮一次的 400 往返。

**漏调工具补救**（`SubAgent::turn`，`sub_agent.cpp:156-162`）

auto 模式下模型可能只输出文字、不调 verb 工具（bigbang 投票会因此判为 dissent，Monica worker 会交不出结构化汇报）。处理：
- 当 `calls` 为空、无 `lastError_`、`text` 非空、且 `r < maxToolIters_ - 1` 时，先把旁白作为 assistant 消息写入，再追加一条 user 消息 `Please call the '<verb>' tool to submit your result.`，继续下一次循环；
- **每次 turn 最多补救一次**，避免死循环；补救后仍无工具调用，按现有逻辑返回旁白。

**配置校验**（`loadFromJson`）
- **已知键、非法值**：解析失败，报错信息列出合法取值。覆盖 `tool_mode`、`converge`、`thinking`、`shape`、`emit`、`when`。
- **未知键**：不中断运行，只告警。
  - 落点：`loadFromJson` 是静态函数，拿不到回调。给 `SkillConfig` 增加 `std::vector<std::string> warnings`，解析时写入；`SkillRunner::run()` 在加载配置后（`skill_runner.cpp:239` 之后）逐条 `cbs_.onEvent("system", "system", ...)` 发出。
  - 告警文本示例：`unknown key 'toolMode' (did you mean 'tool_mode'?)`。
- `max_recursion` 移除后按未知键处理，旧配置只告警。

### 3.3 P3a：引擎并发派活

**1. 并发规则**（`SubAgent::turn`，`sub_agent.cpp:189-193`）

对一次回复里的所有工具调用：
- `request_agent` 按 `agent_id` 分组：**组间并发，组内按出现顺序串行**；
- 其他工具调用保持原顺序串行；
- 全部完成后，按 `calls` 原顺序为**每一个**调用写入 tool result（不能留下没有结果的 tool_call），再发起下一次请求。

**2. 并发上限**：新增技能级配置 `max_parallel`，默认 16，表示同时执行的组数上限，超出的排队。

**3. 一层派发**（`skill_runner.cpp:196-218`）
- **流程驱动者** = 出现在 `round_steps` 或 `on_converge` 中任一 action 的 agent。
- 只给流程驱动者注册 `request_agent` 并挂拦截器。其他 agent 看不到这个工具，即使调用，`dispatchSide` 也会返回 not allowed。
- `handleRequestAgent` 拒绝以流程驱动者为目标的派发，返回 `[error] cannot dispatch to a driver agent`。
- 删除 `static thread_local int depth`（`:206`）、`maxRecursion` 字段及相关逻辑。

**4. 线程安全**
- 每个并发线程体都用 `try { ... } catch (...)` 包住，异常转成 `[error] ...` 作为结果。
- 现有 `parallel` step 线程（`skill_runner.cpp:279`）同样补上。
- 线程开始前检查 `checkCancel()`，已取消则直接返回。
- `cancel()` 遍历所有 agent 调用 `client_.cancel()`，保持不变。

**5. procRunner 补设计注释**（只加注释，不改逻辑）

在 `iprocess_runner.h`、`process_runner_win.cpp`、`process_runner_linux.cpp` 的 `Run()` 处写明：全局只有一个 runner，`Run()` 全程持锁、串行执行是有意为之。外部程序无法可靠接收 ESC，所以同一时间只允许一个子进程，`Cancel()` 只针对当前进程。需要并发的场景应使用内部工具，不要改成多实例或无锁。

**不变**：`round_steps` 执行方式、skill_maker。bigbang 只改 3.2 第 4 条那一行配置。

### 3.4 P3b：Monica 改造

**`skills/monica/config.json`**
- `tools`：`["read_file", "write_file", "edit_file", "grep_files", "file_search", "list_dir"]`，**去掉 `exec_shell`**。
- `agents`：
  - `supervisor`（`supervisor.md`，thinking 保持默认 `on`）；
  - `worker_1` ~ `worker_10`（全部用 `worker.md`，`"thinking": "off"`）。
- 新增 `"max_parallel": 16`；删除 `max_recursion`；`max_rounds` 改为 8。
- 保留 `tool_mode: auto`、`dynamic_dispatch: true`、`dispatch_tool: emit`、`stop_when`、`on_converge`。
- 删除 `formatter.md`、`translator.md`、`auditor.md`，新增 `worker.md`；同步更新 `skills/monica/README.md`。

**`tests/unit/test_skill_monica.cpp`**
- 当前只给 `formatter` / `translator` / `auditor` 设了脚本 responder（`:71-75`）。没设 responder 的 agent 会走真实 API（`sub_agent.cpp:60-96`）。
- 改完后**必须为 `supervisor` 和 `worker_1`~`worker_10` 全部设置 responder**，否则单测会联网。

**`supervisor.md`** 写明以下流程：
1. **分析**：先 `list_dir(depth=2)` 看全貌，再按需加深；配合 `file_search`、`grep_files`、`read_file` 区间读取，产出任务清单。
2. **分批**：
   - 按模块耦合分批，`.h/.cpp` 配对、同目录、同功能的文件放在同一批；
   - 每批 **6~10 个文件**，大文件单独成批；
   - 同一个文件只能出现在一个批次里。
3. **派发**：在**同一个回复中**把批次派给多个空闲 worker，尽量派满。批次多于 worker 时分多次派发，目标 2~4 次。
4. **核对**：收齐报告后核对清单，失败或遗漏的补派。全部完成后回复 `DONE`。
5. 写入两个具体示例：
   - **一次回复同时派多人**：
     ```
     request_agent(agent_id="worker_1", message=<批次1>)
     request_agent(agent_id="worker_2", message=<批次2>)
     request_agent(agent_id="worker_3", message=<批次3>)
     request_agent(agent_id="worker_4", message=<批次4>)
     ```
   - **派活消息模板**：
     ```
     Files:
     - src/core/agent.h
     - src/core/agent.cpp
     Tasks (in order, per file):
     1. format alignment
     2. full-width / Chinese check
     3. unused-code audit (report only)
     Report format: <与 worker.md 中的汇报格式一致>
     ```

**`worker.md`**：
- 只处理派活消息里列出的文件，清单之外一律不碰。
- 每个文件按顺序做三步：
  1. **格式对齐**；
  2. **中文与全角检查**：全角字符改为 ASCII，中文注释译成英文；
  3. **无调用代码审查**：对每个符号**单独**用 `grep_files` 全项目搜索引用，必要时调大 `max_results`；结果出现截断提示时标注"搜索截断，结论不可靠"。结论一律标为"疑似"，**只报告，不删除**。
- **修改方式**：一律用 `edit_file` 做局部修改，不要整文件重写。没有 shell，不要尝试任何 shell 方式（`write_file` 描述中关于 `exec_shell` 的建议对你不适用）。
- 大文件用 `read_file` 区间读取。
- **必须调用 `emit` 工具提交汇报**，不要只输出文字。汇报格式固定：处理的文件、改动摘要、疑似无调用代码（文件:行）、失败项及原因。

---

## 4. 执行顺序

| 阶段 | 内容 | 主要文件 | 依赖 |
|------|------|---------|------|
| P1 | read_file 行区间 + list_dir + grep_files 调整 + format_file | `src/tools/file_tool.cpp`、`src/tools/format_tool.cpp`、`src/core/sub_agent.cpp`、`src/core/config.cpp`、`tests/CMakeLists.txt` | — |
| P2 | thinking + 漏调补救 + 配置校验 + bigbang 配置 | `src/client/models.h`、`src/client/deepseek.cpp`、`src/core/skill_config.h`、`src/core/skill_runner.cpp`、`src/core/sub_agent.*`、`skills/bigbang/config.json` | — |
| P3a | 并发派活 + 一层派发 + 线程安全 + procRunner 注释 | `src/core/sub_agent.*`、`src/core/skill_runner.*`、`src/core/skill_config.h`、`src/platform/iprocess_runner.h`、`src/platform/process_runner_win.cpp`、`src/platform/process_runner_linux.cpp` | P2 |
| P3b | Monica 配置、prompt、测试 | `skills/monica/*`、`tests/unit/test_skill_monica.cpp` | P1、P2、P3a |

每个阶段完成后先跑全部 `tests/unit`，通过后再进入下一阶段。

---

## 5. 测试

**单元测试**（`tests/unit/`）
- **read_file 行区间**：切片、行号、头部信息正确；截断续读行号正确；非法区间报错；不传参数时行为不变；`#L` 写法解析正确。
- **list_dir**：`depth` 生效且未展开目录有标注；`exclude` 正则生效；非法正则报错；工作区外路径被拒绝；跳过 `kSkipDirs`；超过条目上限截断。
- **grep_files**：`max_results` 生效且有上限；截断时有提示；不传参数时行为不变。
- **format_file**：`whitespace` 模式的行首制表符展开/行尾空白/末尾换行/CRLF 保留；clang 二进制不存在时报明确错误且文件不变；`mode` 缺省为 `clang`。
- **thinking**：`on` 时请求体无该字段；`off` 时字段正确；角色级覆盖技能级；`force` 时一律关闭。
- **漏调补救**：首次只回文字时会追加提示并再请求一次；补救最多一次。
- **配置校验**：非法值报错；未知键（含 `max_recursion`）写入 `warnings` 并经 `onEvent` 发出，不导致失败。
- **并发派活**（`test_skill_swarm.cpp`，用 `multiResponder` 模拟）：
  - 派给 4 个不同 worker 时，总耗时接近单个 worker；
  - 同一 worker 的 2 个任务串行；
  - 结果按 `tool_call_id` 和原顺序回填；
  - `max_parallel` 生效；
  - worker 没有 `request_agent`；
  - 以驱动者为目标的派发被拒绝；
  - 线程内抛出异常时程序不崩溃；
  - `cancel()` 能停止所有执行中的 worker。
- **Monica**：新 config 解析通过；所有 agent 均有 responder，测试不联网。

**真机验收**：在 50 个以上源文件的项目上运行 `/monica <project>`。
- 正常收敛并产出最终报告；
- skill round ≤ 6；
- 日志中出现单次回复派给 3 个以上 worker；
- 记录总 API 调用数、token、耗时，与改造前对比；
- 每个文件都能在某个 worker 的报告中找到记录；
- 运行中按 ESC 能在数秒内停止。

**回归**：
- `bigbang`：仍能收敛，投票为结构化结果，界面上有思考链，不再出现 `tool_choice` 兜底重试日志；
- `skill_maker`：行为不变。

