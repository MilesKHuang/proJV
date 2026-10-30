# Ostlr — 需求与初步设计（v0.3）

> 状态：**架构已定，进入 Phase 0 验证**。产品名见 §1.1。
> 本文件是独立项目的设计基线；独立仓库建立后整体迁出。

---

## 1. 定位

**多窗口 AI Agent 终端工作台**：原生 C++ 跨平台桌面应用，自己拥有 PTY 与终端渲染，把多个 agent CLI（Claude Code / Codex / proJV / 任意 shell）装进统一分屏工作区，并在侧栏提供文件、进程、资源与状态管理。

> 不是 proJV 的子项目，不依赖 proJV，可 spawn 任意 CLI。

### 1.1 命名

**Ostlr**（定名）。取"ostler（马夫/管马厩的人）"之意，去掉元音形成品牌化拼写：
一个人管理一厩 agent，与本产品"多 pane 统一调度"的定位一致。

- 读作 "OST-ler"（/ˈɒslər/），与 ostler 同音。
- 含义：**管马厩的人**——不是马本身。产品角色是"调度与管理 agent 的人"，而非再做一个 agent。
- 曾用名 Mews / Maiws 已废弃。

---

## 2. 要解决的问题

| 痛点 | 现状 | Ostlr |
|---|---|---|
| 多窗口项目失忆 | 多个 agent 窗口，记不清哪个是哪个项目/模型/分支 | pane 绑定工作区路径 + 启动配置 |
| 注意力分散 | 不知哪个在跑、哪个在等我、哪个卡住 | 侧栏状态汇总 working / waiting / blocked / idle / error |
| **文件操作断层** | 路径靠手打/复制，文档树看不见 | **文件系统树 + 一键复制路径 + 点击插入路径** |
| **图像引用困难** | TUI 无法把图片喂给 agent | **文件树按钮 → 向 PTY 注入 `@<path>`** |
| **资源不可见** | 多个 agent 跑起来看不到谁吃 CPU/内存 | **侧栏资源占用（进程级）** |
| **进程管理靠系统** | 只能切系统窗口，杀进程要开任务管理器 | **侧栏 TUI 进程列表：切换 / 终止 / 重启** |
| 窗口管理靠系统 | 平铺 WM 做不好分屏与持久化 | pane 树 + 标签 + 布局持久化 |
| 多厂家割裂 | 每家配置/会话各一套 | 统一读取展示 |

---

## 3. 最终形态

- **单原生窗口**：中间 **终端网格**，右侧 **可折叠 Side Panel**（不要左右双栏，信息过载）
- Side Panel 四块：**文件系统树** / **pane 状态** / **资源占用** / **TUI 进程管理**
- 每个 pane 是真实 PTY 会话，跑真 CLI，**与系统终端行为一致**
- pane 拖拽分屏、切标签、保存布局，重启恢复
- 支持中文输入法、图片协议、复制粘贴、搜索
- Windows + Linux 优先，macOS 后续

**不做**：不做通用终端模拟器去卷 Ghostty/WezTerm；不追求 100% 终端兼容；不做云端协作；不做 TUI 表达力的全量替代（只做侧栏增量）。

---

## 4. 技术架构

| 层 | 选型 | 理由 |
|---|---|---|
| GUI 外壳 | **ImGui docking 分支** | docking 直接给 pane 树、分屏、标签、布局持久化 |
| 文件系统树 | **现成 ImGui 文件浏览器实现**（如 ImGuiFileDialog 一类的树控件） | 不重复造轮子；只做交互接线 |
| 终端状态 | **libghostty-vt**（C ABI，Zig 构建） | 成熟 VT 解析、屏幕状态、脏区追踪、按键编码 |
| 字形渲染 | **FreeType + HarfBuzz + 自建图集；复用 proJV 的 ttc 加载机制** | proJV 已用 `projv_files/fonts/*.ttc` 解决 CJK，直接沿用同一套加载与回退逻辑 |
| 平台/输入 | ImGui win32 后端（IMM32）或 SDL3 后端 | 取决于 Phase 0 IME 验证结果 |
| PTY | `forkpty`（Linux）/ `CreatePseudoConsole`（Win） | 各 200–400 行薄封装 |
| 状态检测 | 屏幕尾部正则规则表（借鉴 Herdr manifest） | 按优先级/区域/排除条件匹配，无需 agent 配合 |
| 构建 | CMake + 全 vendored | 与 proJV 同哲学：无包管理器 |

**关键分层**：ImGui 只管**外框**（pane/标签/面板/文件树）；终端格子 = libghostty-vt 出状态 + 自建图集出字形 + `ImDrawList::AddImage` 贴图。**不使用 ImGui 字体系统与 InputText 控件。**

**输入链路**：内层 TUI 没有 IME，只收 UTF-8 字节。组字、候选框、上屏全部在外层 GUI 完成——本应用必须自己解决的硬问题。

---

## 5. Ostlr ↔ TUI 协作协议

两个约定，需要 TUI 侧（优先 proJV）跟进：

### 5.1 工作区指定

Ostlr 启动 pane 时用：

```
cd <workspace_path> && TUI_EXECUTION_FILE=<file> <cli>
```

- `cd` 决定工作区根目录（多 pane 各指各的目录）
- `TUI_EXECUTION_FILE` 显式告知"本次执行的入口文件"，避免 TUI 自己猜
- **TUI 侧要求**：读取 `TUI_EXECUTION_FILE` 环境变量作为执行上下文；未设置时回落当前目录
- 好处：不依赖任何厂商私有协议，纯环境变量 + cwd，天然跨厂家

### 5.2 文件/图像引用注入

Side Panel 文件树每个条目带一个"引用"按钮：

- 点击 → 向**当前聚焦 pane 的 PTY** 写入 `@<绝对路径>`
- **TUI 侧要求**：解析输入中的 `@<path>`，图片按图像输入处理，其它按文件引用处理
- 这样把"TUI 无法插图"变成"点一下按钮"，不需要拖拽协议、不需要剪贴板 hack

---

## 6. 已定设计决策

| # | 决策 | 理由 |
|---|---|---|
| 1 | 独立项目、独立仓库 | 通用工具带动生态，不绑死 proJV |
| 2 | 架构 A：自己拥有 PTY 并自己渲染 | 只有这样才能拿到精确 pane 状态 |
| 3 | 不引入 Electron / Web 栈 | 单二进制、低内存、无 runtime |
| 4 | VT 层用 libghostty-vt，不自己写 | 省最大一块工作量；导出 C ABI，官方带 CMake 包装 + 28 个 C 示例 |
| 5 | 字形层自建，但复用 proJV 的 ttc 机制 | CJK 回退链与 shaping 是硬需求，机制已验证 |
| 6 | 文件树用现成 ImGui 实现 | 不造轮子 |
| 7 | 只做一个 Side Panel | 双栏信息过载，一个够用 |
| 8 | 状态检测靠屏幕规则表 | 跨厂家通用的唯一现实路径 |
| 9 | 与 TUI 的协作只走环境变量 + `@path` | 最小侵入、跨厂家、可失败降级 |
| 10 | Windows 优先，Linux 同步 | 用户主战场 + CI 双平台 |

---

## 7. 风险与 Phase 0 闸门

| 验证项 | 硬标准 | 不过怎么办 |
|---|---|---|
| **中文 IME** | 微软拼音/搜狗 + fcitx5：组字有 preedit、提交不丢字、候选框跟光标、组字期间按键不漏进 PTY | **硬闸门** → 退回 SDL3 原生 + 自建 UI |
| libghostty-vt 出画面 | `vim` / `htop` 与真终端逐格一致 | 评估替代 VT 库 |
| Windows ConPTY | 同一套跑通 | 调整 PTY 封装 |
| 渲染性能 | 全屏滚动 60fps，CJK 不错位 | 优化图集与脏区重绘 |

**已知风险**
- libghostty-vt 自述 API "incomplete, not yet stable" → 锁版本 + vendor 源码
- Zig 构建依赖 → 预编译各平台静态库随仓库分发
- `@path` / `TUI_EXECUTION_FILE` 需 TUI 侧配合，短期只有 proJV 可用

### 7.1 复杂度评估（基于 herdr 源码实测）

herdr 生产代码 **257k 行**（不含 12.7k 行测试），但同时做了终端模拟器 + TUI 客户端 + 服务端 + 远程 + 多 agent 集成。裁剪后：

| 模块 | herdr | Ostlr | 说明 |
|---|---|---|---|
| Windows 输入 | 9.3k 行 | **必须自研** | 唯一硬骨头，`windows_vti.rs` 4.1k + `platform/windows.rs` 5.2k；可参考 herdr（Apache-2.0） |
| VT 状态/渲染 | 12.8k 行 | ≈0 | 全部由 libghostty-vt 承担 |
| 平台 PTY | 4.1k 行 | 可控 | C++ forkpty / ConPTY 比 Rust 封装更直接 |
| server/client 协议 | 15k 行 | **砍掉** | 单进程 GUI 直接持有 PTY，无需 IPC |
| agent 集成 / hook | 11k 行 | 砍掉或极简 | 不注入各家 hook |
| 远程 / SSH / worktree | 7k+ 行 | 砍掉 | 非目标 |
| TUI 界面 | 3.4k 行 | 换 ImGui | 外框用 ImGui docking |
| 状态检测规则 | 5.2k 行 | 收缩 | 只覆盖目标 CLI |

**结论**：预估生产代码 8k–15k 行，Windows 输入占 30–40%。不是长尾地狱，是"一个硬骨头 + 其余可控"。

**消费 libghostty-vt 的方式（已核实官方支持）**：Ghostty 官方仓库自带 CMake 包装，`FetchContent` + `target_link_libraries(app PRIVATE ghostty-vt-static)` 即可；另有 28 个官方 C 示例（`example/c-vt-render`、`c-vt-encode-key`、`c-vt-paste` 等）可直接对照。构建机需 zig，但可预编译静态库分发，终端用户无需 zig。

---

## 8. 里程碑

| 阶段 | 内容 | 预估 |
|---|---|---|
| Phase 0 | IME 与 VT 渲染验证（不做产品） | 1–2 周 |
| Phase 1 | 单 pane MVP：PTY、格子渲染、滚动、复制粘贴、ttc 字体 | 3–4 周 |
| Phase 2 | 多 pane + Side Panel：分屏、标签、布局持久化、文件树、状态、资源、进程管理 | 4–6 周 |
| Phase 3 | 协作协议：`TUI_EXECUTION_FILE`、`@path` 注入、多厂家配置模板 | 待定 |

> 预估为相对量级，非交付承诺。
