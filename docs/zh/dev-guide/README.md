# proJV 开发指南

> [English](../../en/dev-guide/README.md) | **中文**

> 读完这页你能：编译、跑测试，并且知道改某个功能该开哪几个文件。

## 一次请求的路径

```
输入框
   |  Agent::submitUserMessage
   v
Agent（src/core/agent.*）  —— 会话、上下文预算、压缩
   |  ChatRequest
   v
DeepSeekClient（src/client/deepseek.*）  —— libcurl、SSE、重试
   |  StreamCallbacks：text / thinking / tool_call
   v
Agent 回合循环  —— 审批？-> ToolRegistry::execute -> 工具结果
   |                     （或：进入技能阶段，见下）
   v
ChatView / 气泡（src/tui/*）  <-- onEvent / onProgress
```

技能是一个独立阶段：工具阶段结束后，`TuiApp::runSkillBlocking` 创建 `SkillRunner`，
它持有一组 `SubAgent`。每个 `SubAgent` 跑自己的 LLM 循环；runner 解释
`config.json`（轮次、动作、槽位、投票），把 `[角色]` 消息发进同一个聊天区。

## 编译与测试

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

- 测试用 doctest：`tests/unit/` 测逻辑，`tests/golden/` 在内存里渲染 FTXUI 界面
  （不需要真终端）。
- `PROJV_BUILD_TESTS=ON` 是默认值；发布包不带测试。
- CI（`.github/workflows/ci.yml`）在 `windows-latest` + `ubuntu-latest` 上跑上面
  三条命令；打 `v*` 标签时走 `release.yml`，额外用 CPack 打包。

## 目录结构

```
src/
├── tui/        入口、应用状态、聊天视图、Markdown、主题、状态栏、TODO
├── core/       agent、session、config、storage、prompts、项目上下文、
│               技能引擎（skill_runner / sub_agent / skill_config）
├── client/     DeepSeek 客户端、请求/响应模型
├── tools/      注册表 + 每个内置工具一个文件、web 工具、pytool 管理
└── platform/   进程 runner 与系统工具（Windows / Linux）
tests/          doctest 单测 + golden 渲染测试
skills/         示例技能（bigbang、monica），构建时部署到可执行文件旁
projv_files/    运行时数据：fonts、theme、pytool、skills（config/prompts/
                sessions 首次启动时生成，永不打包）
external/       内置依赖：FTXUI、SQLiteCpp、curl、json、toml++、loguru、doctest
```

## 依赖

| 库 | 用途 | 位置 |
|----|------|------|
| FTXUI 7.0.3 | 终端界面 | `external/` |
| SQLiteCpp 3.3.3 | 会话存储 | `external/` |
| libcurl 8.21.0 | HTTP | Windows 在 `external/`，Linux 用系统库 |
| nlohmann/json | JSON | 单头文件 |
| toml++ | config.toml | 单头文件 |
| loguru | 日志 | `external/loguru.cpp` |
| doctest | 测试 | 单头文件 |

不需要 vcpkg / conan / npm / pip。

## 真正要紧的代码约定

- **线程模型。** FTXUI 只在 TUI 线程。耗时活儿（API、工具、技能）在 worker 线程。
  UI 回调（`onEvent`、`onProgress`）只在 join 之后、在 runner 线程调用，worker 线程
  绝不直接调。
- **取消。** `Esc` -> `Agent::cancel()` -> `ToolRegistry::cancelAll()` ->
  `IProcessRunner::Cancel()`；技能另有一条 cancel probe
  （`SkillRunner::setCancelProbe`），因为 TUI 摸不到 runner 对象。
- **只有一个进程 runner。** `Run()` 全程持锁是刻意的，不要改成进程池；需要并发的
  场景用内部工具。见 `src/platform/iprocess_runner.h` 的注释。
- **错误格式。** 工具返回字符串；错误以 `Error:` 开头（技能内部是 `[error]`），
  这些文本是给模型读的。
- **测试缝。** `SubAgent` 支持脚本化 responder（`setResponder` /
  `setResponderMulti` / `setResponderText`），所以技能测试不联网。
- **编码。** 源码 UTF-8；MSVC 需要 `/utf-8`（已按 target 设置）。

---

接着读 -> [模块地图](module-map.md) | 改动配方 -> [recipes](recipes.md)
