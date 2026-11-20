# studio（GUI 前端）

> 读完这一页，你能构建这个窗口、知道某个元素在哪个文件里画、并且能证明一次改动
> 没有让 GUI 与 TUI 产生差别。

proJV 现在是**一个引擎、两个窗口**：

```
SessionRuntime (src/core/)  -- submit / run / cancel，事件流出。不含视图。
        |
        +-- proJV_tui     FTXUI     src/tui/
        +-- proJV_studio  Dear ImGui studio/
        |
   两者都经由 view_model::Source -> Screen -> 一份 JSON 快照
```

studio 不是第二个产品，而是同一状态的第二个窗口；共享的投影层保证它一直是。

## 构建与运行

两个可执行文件都**始终构建**，GUI 没有开关。

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
# Windows
build/Release/proJV_studio.exe
# Linux（SDL2 是构建期依赖：sudo apt-get install -y libsdl2-dev）
build/proJV_studio
```

安装器的 `projv-studio` 转发器打开窗口，`projv` 仍然打开 TUI。
`--dump-view-model [file]` 输出一帧快照后退出——这是"窗口显示了终端没有的东西"
的排查入口。

## 视图模型（设计 08.5 D4）

`src/core/view_model.h` 就是契约：

| 组成 | 含义 |
|---|---|
| `view_model::Source` | 前端填的纯数据：气泡、Agent 状态、各面板行、模态、输入行 |
| `view_model::build` | 必须与 TUI 逐字节一致的推导 |
| `view_model::toJson` | 整屏一份 JSON 对象，键有序，UTF-8 不转义 |

只存在于这一处（前端不得再写一遍）的推导：

- 气泡 -> 聊天行，规则来自 `chat_view.cpp`：`── You ──` 一类标签、思考的 18 行
  尾窗、工具结果 5 行截断、tool_call 与 tool_result 合并、投票配色、
  `[Context compacted:` 的警告色；
- 状态行（`status_line::render`）与状态栏分段（`status_bar::renderSegments`）；
- `/` 菜单窗口（可见 10 行，跟随选中）；
- 键帽提示条。

前端只负责**版面与输入**，不负责文案。标签错了就是两边一起错，这正是目的。

## 版面一致性

| TUI | studio |
|---|---|
| 标题 `proJV (TUI)` + `[Agent: phase]` | `proJV (studio)` + 同一行 phase，同一主题字段 |
| 活动条：39 列单元、自动换行 | 同一单元、同一宽度，按窗口宽度换行 |
| 聊天：带框块、markdown、思考卡、diff 块 | 同一批行，用 ImGui 画；markdown 用同一个 `markdown_text` 解析器 |
| 右 dock 36 列：TODO + 后台任务 | 同样的切分与宽度 |
| 状态行 + 转轮 + `[skill]` | 相同；转轮是同样四帧字形 |
| 输入框上方的 `/` 菜单 | 同样的行、同样的 10 行窗口 |
| 输入行 | 单行 ImGui 输入框，带输入法 |
| `Queued: N message(s)` | 同一行、同一颜色 |
| 状态栏分段 + 角色胶囊 | 同样的分段与颜色 |
| 键帽提示 F1..Esc | **不再有键帽条**：右上角按钮各自带快捷键（悬停显示），完整清单在帮助对话框里。同一张键位表写两遍，只会像没做完 |

## 按键

与 `docs/*/user-guide/08-reference.md` 完全一致：F1 帮助、F2 配置、F3 新会话、
F4 打开会话、F5 MCP、F6 主题、F7 主题编辑器、F8 展开思考、F9 dock、F10 模型、
Tab 切角色、Up/Down/PgUp/PgDn 滚动、Enter 发送、Esc＝取消／清空／询问退出、
Ctrl+C 永不退出。

应用自己的按键表在**任何控件提交之前**运行（`App::handleKey`），所以获得焦点的
输入框吞不掉 Tab 与 F 键。输入行有焦点时方向键滚动聊天，在多行框里则是移动光标。

## 模态

十个，与 TUI 的十个一一对应：approval、config、help、quit、welcome、MCP、
打开会话、模型、主题、主题编辑器。同时只有一个；打开时后面的界面不接受输入。
Esc 关闭除审批以外的全部——破坏性命令要一个明确答复。没有 API key 时，
欢迎页上的 Esc 直接退出（此时还没有可输入的东西）。

## 字体、输入法、hi-DPI（设计 D7）

- 等宽在前、CJK 合并在后：Windows 用 Consolas + 微软雅黑，Linux 用
  DejaVu Sans Mono + Noto Sans Mono CJK SC。换成比例字体，TUI 依赖的框线表格
  会整片错位。
- `io.ConfigDpiScaleFonts` 加 `style.FontScaleMain` 跟随显示器 DPI；ImGui 1.92+
  按实际字号重新栅格化，125% / 150% 依然清晰。
- 输入法候选窗通过 `ImGui::GetPlatformIO().Platform_SetImeDataFn` 跟随光标
  （见 `studio/main_studio.cpp` 的 `setImeData`）。imgui_impl_win32 1.93 不带
  这个回调，缺了它候选窗会停在屏幕角落。
- 粘贴与 TUI 同样处理：制表符与换行变成空格，超长截断，粘进来的块不会自己发出去。

## 设计系统（重建后的界面）

窗口不再靠"每个调用点自己写数字"摆版面，而是三层：

| 层 | 文件 | 负责 |
|---|---|---|
| 令牌 | `studio/design/tokens.{h,cpp}` | 唯一的尺寸阶梯：字号 28/22/18/16/13.5/12.5、间距 0/4/8/12/16/24/32、圆角 4/8/12/16/胶囊，按（显示器 DPI × 用户缩放）解析成 `Metrics` |
| 布局 | `studio/design/layout.{h,cpp}` | Flexbox 子集：方向、间隙、内边距、Fixed/Fit/Grow/Percent、min/max、交叉轴 Align、主轴 Justify。纯算术，不依赖 ImGui |
| 控件 | `studio/ui/ui.{h,cpp}` | 一套调色板 + 一套控件：按钮四种权重、勾选、单选、分段选择、输入框、列表行、卡片、徽章、键帽、滚动区 |

规矩只有三条：

1. `studio/` 里没有第二个人写 `12.0f * dpi`——要间距就写 `tokens::spacePx(m, Space::Md)`。
2. 界面上的颜色只能来自 `ui::palette()`（它从 `ThemeColors` 推导），`studio/ui/` 之外不出现 RGB。
3. 字要放得进去：单行文字一律先量再画（`fit::ellipsize` / `fit::bestTier`），标题在空间不够时自动降一档。

### 帧几何

`drawFrame` 每帧先算完所有区块再画：

```
window
├── header    标题、阶段徽章、模型/会话/配置/帮助按钮
├── body      Row [ 聊天区(Grow) | dock(Fixed) ]   dock 可用 F9 关闭
├── composer  输入卡片，与聊天列的左边缘对齐
├── footer    状态行、排队行、服务栏、键帽栏
└── slash     锚在输入框上方的 "/" 菜单浮层
```

高度全部由本帧内容量出来：键帽换几行、dock 开不开、有没有排队行，都会改变结果。
这就是窗口缩放不再"慢一帧"、也不再需要 `static float bottomHeight` 的原因——
那类上一帧残留状态在这个文件里已经一个不剩。

## 一致性测试

- `tests/unit/test_view_model.cpp` 钉住推导，并与 TUI 自己的渲染器交叉验证：
  视图模型产生的每个标签，必须逐字出现在 `chat_view::renderBubbles` 对同一批
  气泡的输出里。
- `tests/unit/test_studio_design.cpp` 钉住几何：字号阶梯单调、缩放被夹住、
  行/列/Percent/Grow/min-max 的矩形、CJK 算两列、省略号不切断码点、
  `bestTier` 选最大能放下的档位。三个模块都不含 ImGui，所以不需要 GPU 也能测。
- `tests/unit/test_worker_wait.cpp` 钉住退出语义：干完的线程会被 join（可以安全清理），
  没干完的线程被 detach 且调用立刻返回（`src/core/worker_wait.h`）。没有"等 N 秒"这种判据。
- `tests/unit/test_status_line.cpp` 钉住 `Stopping...`：取消请求压过所有运行中的状态，
  但空闲时不会显示（两端共用这一条推导）。
- `proJV_studio --dump-view-model screen.json` 写窗口快照；
  `proJV_tui run --jsonl` 写同一会话的事件流。两者对照，就是"不靠人眼看屏幕"
  找行为差异的方法。

## 有意保留的差异

- **停靠开着，但还没有 dock 宿主。** 设了 `ImGuiConfigFlags_DockingEnable`、
  关掉了多视口（D1）；版面仍是 TUI 的固定版式，浮动面板等 08.7 蓝图编辑器要用
  时再接。
- **窗口有鼠标，也有选区模型。** 聊天区文字可拖选并 Ctrl+C 复制
  （`studio/studio_text.cpp`）；`/` 列表是锚在输入行上方的浮层窗口，永远不会把
  输入框挤出窗口；模态对话框带 Close/OK 按钮。这些都是键位表之上的补充，从不替代它。
- **十个模态共用一个外壳。** 标题栏（含关闭按钮）、可滚动正文、右对齐动作页脚，
  尺寸随内容长；按钮里主操作用强调色。以前十种对话框十种尺寸，在 150% 下会裁字。
- **次要文字以可读性优先。** `studio_theme::readable()` 把次要文字提到对窗口底色
  至少 4.5:1 的对比度。Light 主题里那档"禁用灰"只有 2.7:1，在终端里没问题，
  在窗口里看不清。
- **版面按行缓存**，长会话不再每帧重解析——正是那份二次复杂度让技能运行时窗口
  看起来"没响应"。
- **不写 `imgui.ini`。** 版面固定，没有需要漂移的状态文件。
- **正文字号 16px（100% 时）。** 上一版是 14.5，在 4K/100% 上要凑近看；整套阶梯
  按 ~1.2 的比例重排，先保证能读，再谈层级。
- **退出不定死时间，清理不了就不清理。** `SessionRuntime::shutdown()` 只问一句：
  工作线程结束了没有？结束了就 join，然后关库、收子进程、关 MCP；没结束就 detach，
  **什么都不清理**——因为线程可能还在写会话库，在线程还活着时关库就是 use-after-free
  （`src/core/worker_wait.h`）。剩下的交给操作系统回收，SQLite 下次启动重放 WAL。
  上一版是“等 5 秒再照常清理”，两头不讨好：既像随机卡顿，又踩了那个竞态。
- **ESC 立刻可见。** `Agent::cancel()` 置 `AgentStatus::cancelling`，
  `status_line::render` 把它渲染成 `Stopping...`，两端在下一帧就能看到“请求已收到”。
- **回合路径里的每个阻塞等待都可取消。** 并行工具抢槽原来是 `cv_.wait`，从不看取消位，
  停在那里的线程叫不醒；现在等的是“有槽位 **或** 已取消”
  （`src/core/sub_agent.cpp`），与审批等待同一个范式。
- **输入框是一个控件，不是“卡片套输入框”。** 输入框自己的框就是那个框：主题底色、
  一条 hairline 边框、token 圆角与内边距，并把 `ImGuiCol_NavCursor` 推成透明；
  键盘焦点只取一次（每帧抢焦点会让窗口里其他控件都按不下去）。
  之前是卡片 + ImGui 焦点高亮，两圈必然对不齐。

## 不靠人眼测试这个窗口

- `build/studio.ps1` 用 `Start-Process` 启动/停止。直接用 `start` 会让 GUI 继承
  shell 的 stdout 管道，命令要等到窗口关闭才返回——这就是"kill 没生效"的真相。
- `build/shot.ps1` 截图前先声明 per-monitor DPI awareness。DPI-unaware 的
  PowerShell 在 4K/150% 屏上拍到的是缩放到 2560x1440 的桌面，底栏会缩成几粒近黑像素。
- `build/drive.ps1` 注入按键前把目标窗口切到 en-US。中文输入法激活时，预编辑会吞掉
  注入的字符。
- `PROJV_STUDIO_DEBUG_LAYOUT=1` 会写 `layout.log`（各区块光标位置、clip 矩形、
  帧心跳）与 `wnd.log`（原始 Win32 按键消息）。这一阶段每个版面与输入缺陷都是靠
  这三个文件定位的。

## 文件

| 文件 | 负责 |
|---|---|
| `studio/main_studio.cpp` | Windows：WGL 上下文、输入法钩子、主循环 |
| `studio/main_studio_sdl.cpp` | Linux：同一套循环，走 SDL2 |
| `studio/studio_app.{h,cpp}` | 状态、动作、按键表、填充 `Source` |
| `studio/studio_ui.cpp` | 主界面：聊天、dock、状态、输入、键帽 |
| `studio/studio_modals.cpp` | 十个模态 |
| `studio/studio_markdown.cpp` | `markdown_text` 行 -> ImGui 绘制 |
| `studio/studio_theme.cpp` | `ThemeColors` -> ImU32（studio 唯一的颜色转换）、字体 |
| `studio/design/tokens.{h,cpp}` | 尺寸阶梯（字号/间距/圆角）与 `Metrics` |
| `studio/design/layout.{h,cpp}` | Flexbox 子集求解器，纯算术 |
| `studio/design/fit.{h,cpp}` | 量宽、省略号、按档位自适应，量测函数外部注入 |
| `studio/ui/ui.{h,cpp}` | 调色板 + 控件工具箱（按钮/勾选/单选/分段/输入/列表行/滚动区） |
| `src/core/view_model.{h,cpp}` | 上面的契约 |
| `src/core/speaker_slots.{h,cpp}` | 两端共用的角色配色槽 |