# proJV GitHub 文档装修规划

> 状态：**D1 / G1 / D2 / D3 / D4 / G2 / G3 已执行**（v0.6.0）。D5 素材待录制。
> 执行记录见文末第 10 节。

---

## 0. 目标与定位

文档是**路标，不是百科**。每一页只回答三个问题：

1. 这是什么、能做什么（一两句）；
2. 怎么一步步用起来或改起来（编号步骤）；
3. 想深入去哪看（链接到下一页，或直接指向代码/配置文件）。

细节不在文档里重复，直接指向源文件。这样代码改了，文档也不容易过时。

三层入口：

| 读者 | 想要什么 | 去哪里 |
|------|---------|--------|
| 路过的人 | 30 秒看懂 proJV、被吸引 | `README.md` / `README_zh.md` |
| 使用者 | 从小白照步骤用起来，再自己深入 | `docs/<lang>/user-guide/` |
| 开发者 | 看懂模块，知道改某功能看哪几个文件 | `docs/<lang>/dev-guide/` |

---

## 1. 总体约定

- **中英双份**，结构完全一致：
  - 入口：`README.md`（英文）、`README_zh.md`（中文），顶部互链；
  - 文档：`docs/en/` 与 `docs/zh/` 镜像，文件名相同，每页顶部互链对应语言版本。
  - 同一页的中英文必须在同一次改动里一起更新。
- **纯 Markdown**，不使用文档站，在 GitHub 上直接浏览。
- **篇幅**：用户指南每页控制在一屏到两屏；超过就拆页或改成指向源文件。
- **每页结尾固定有"下一步"**：
  - 用户指南：`想深入 →`（下一篇用户页）、`想改它 →`（dev-guide 对应模块）；
  - 开发指南：`相关模块 →`。
- **路径与命令必须可复制运行**，写完后逐条核对。

---

## 2. 目录结构

```
README.md                      # 英文展示入口
README_zh.md                   # 中文展示入口
CONTRIBUTING.md                # 贡献指南（英文，路标式）
CONTRIBUTING_zh.md             # 贡献指南（中文）
.github/
├── workflows/
│   ├── ci.yml                 # push / PR：构建 + 单测
│   └── release.yml            # 打 tag v*：构建并上传 Release 包
├── ISSUE_TEMPLATE/
│   ├── bug_report.md
│   └── feature_request.md
└── PULL_REQUEST_TEMPLATE.md
assets/
├── logo.*                     # 用户提供的现有 Logo（路径待确认）
├── social-preview.png         # 1280×640 分享卡片图
├── raw/                       # 用户录制的原始视频（不进最终页面）
└── *.gif                      # 由视频转换得到的 GIF
docs/
├── en/
│   ├── user-guide/
│   │   ├── README.md          # 目录 + 推荐阅读顺序
│   │   └── 01-install.md … 08-reference.md
│   └── dev-guide/
│       ├── README.md          # 架构总览 + 构建测试
│       ├── module-map.md
│       └── recipes.md
└── zh/                        # 与 en 完全镜像
```

---

## 3. README 装修方案

### 3.1 页面结构（从上到下）

1. **Hero 区**：Logo + 项目名 + 一句话定位 + 徽章（CI 状态、最新 Release、License、平台、C++20、二进制约 3MB）+ 主 demo GIF。标题下第一行放 "Every line of code in proJV was written by proJV"，与"约 3MB、零依赖"并列作为钩子。
2. **三个亮点**：
   - Native C++，约 3MB，零包管理器；
   - 多 Agent 技能：数据驱动，复制文件夹即安装；
   - 每一层都可定制：角色、工具、主题、技能。
3. **3 步上手**：从 Releases 下载并解压 → 填 API Key → 发第一条消息。每步一行命令或一张图；"自行构建"作为备选链接到 `01-install.md`。
4. **功能速览**：一张表，每个功能一句话，并链接到用户指南对应页。
5. **对比表**：见 7.2。
6. **导航区**：三个入口——开始使用 / 玩转技能 / 参与开发（参与开发链接到 `CONTRIBUTING.md`）。
7. **理念**：保留现有 Why proJV（Native C++、No-MCP、DeepSeek），精简；措辞正面表达能力（如"为 DeepSeek 深度优化、成本极低""两个文件即可写一个 Python 工具"），不写"不支持什么"。
8. **致谢 / 赞助**：保留。

### 3.2 移出 README 的内容

| 内容 | 移到 |
|------|------|
| 快捷键表、`/` 命令 | `user-guide/08-reference.md` |
| Windows / Linux 构建细节 | `user-guide/01-install.md` |
| pytool 示例 | `user-guide/07-pytool.md` |
| 技能目录结构 | `user-guide/06-skills.md` |
| 目录树、依赖表 | `dev-guide/README.md` |

---

## 4. 用户指南（`docs/<lang>/user-guide/`）

| # | 页面 | 读完能做到 |
|---|------|-----------|
| 01 | install | 从 Releases 下载运行（首选），或在 Windows / Linux 上自行构建；处理 SmartScreen / Defender 拦截和 Linux 依赖 |
| 02 | first-run | 填 Key 和工作区，认识界面，发第一条消息，看懂工具审批、思考链、取消 |
| 03 | roles | 用 Tab 切换角色，写一个自己的角色 `.md` |
| 04 | images | 用 `@path` 给 Flash 模型发图片 |
| 05 | daily | 会话管理、TODO 面板、主题切换与编辑 |
| 06 | skills | 运行 bigbang / monica，看懂侧边栏，用 skill_maker 做自己的技能 |
| 07 | pytool | 写一个 Python 工具并让 AI 调用 |
| 08 | reference | 快捷键、`/` 命令、`config.toml` 各项、常见问题 |

**每页模板**

```
# 标题
> 读完这页你能：……

## 前置条件
## 步骤      （1. 2. 3. …… 每步写预期结果，必要时配图）
## 怎么确认成功
## 想深入 → / 想改它 →
```

---

## 5. 开发指南（`docs/<lang>/dev-guide/`）

### 5.1 页面

| 页面 | 内容 |
|------|------|
| `README.md` | 一张请求链路图（输入框 → Agent → API → 工具 → 界面）、目录树、依赖、构建与测试命令、代码约定（线程模型、取消、错误返回格式、测试缝 responder） |
| `module-map.md` | 模块地图（见 5.2），每个模块写成一张"路标卡片" |
| `recipes.md` | 常见改动的分步配方（见 5.3） |

### 5.2 模块地图

| 模块 | 主要文件 | 测试 |
|------|---------|------|
| 入口与 TUI | `src/tui/main_tui.cpp`、`app_tui.*`、`chat_view.*`、`status_bar.*`、`approval_logic.*`、`config_view.*` | `tests/golden/*`、`test_approval_logic.cpp` |
| Markdown 渲染 | `src/tui/markdown_*.*`、`bubble_model.*` | `test_markdown_text.cpp`、`test_bubble_model.cpp` |
| 主题 | `src/tui/theme_*.*`、`speaker_colors.*` | `test_theme_*.cpp`、`test_speaker_colors.cpp` |
| Agent 核心 | `src/core/agent.*`、`session.*`、`prompts*`、`project_context.*`、`image_input.*` | `test_agent_request.cpp`、`test_turn_end.cpp`、`test_image_input.cpp` |
| API 客户端 | `src/client/deepseek.*`、`models.*` | `test_deepseek_sse.cpp`、`test_reasoning_content.cpp` |
| 工具 | `src/tools/registry.*`、`*_tool.cpp`、`web_tools.*`、`python_tool_manager.*` | `test_web_api.cpp` |
| 技能引擎 | `src/core/skill_runner.*`、`sub_agent.*`、`skill_config.h`、`skill_trigger.*`、`skills_builtin.*` | `test_skill_*.cpp` |
| 存储与配置 | `src/core/storage.*`、`config.*` | — |
| 平台抽象 | `src/platform/*` | — |

**每张卡片固定格式**：
- 一句话职责；
- 入口文件和关键函数；
- 数据从哪里进、到哪里出；
- 改它时要一起看的文件；
- 相关测试；
- 相关模块 →。

表中文件以执行时的代码为准，05 落地后先重新核对。

### 5.3 改动配方

| 配方 | 涉及模块 |
|------|---------|
| 新增 C++ 内置工具 | 工具、Agent 核心 |
| 新增 pytool | 工具 |
| 新增多 Agent 技能 | 技能引擎 |
| 新增 `/` 命令 | TUI、Agent 核心 |
| 新增配置项 | 存储与配置、TUI 配置页 |
| 新增或修改界面面板 | TUI |
| 接入新的 API 参数 | API 客户端 |

每个配方的格式：按顺序列出要改的文件 → 每个文件改什么 → 怎么写测试 → 怎么验证。

---

## 6. 素材（视频 → GIF）

**用户录制**，放到 `assets/raw/`，文件名按下表：

| 文件名 | 内容 | 建议时长 | 用在 |
|--------|------|---------|------|
| `hero.mp4` | 提问 → AI 调工具 → 流式回答，展示核心体验 | ≤ 20s | README Hero |
| `first-run.mp4` | 首次启动、填 Key、发第一条消息 | ≤ 15s | 3 步上手、02 |
| `skill-monica.mp4` | `/monica <dir>`，侧边栏多 worker 并发 | ≤ 20s | 亮点、06 |
| `skill-bigbang.mp4` | bigbang 辩论到投票收敛 | ≤ 20s | 06 |
| `theme.mp4` | 切换主题、打开编辑器改色 | ≤ 10s | 05 |
| `image-input.mp4` | `@path` 发图片并得到回答 | ≤ 10s | 04 |

录制建议：终端窗口固定大小（例如 120×36），字体放大，避免出现 API Key 等敏感信息。

**下一个 Agent 转换**：用 ffmpeg 把视频转成 `assets/<同名>.gif`。要求：
- 宽度 720px；
- 帧率 10~15fps；
- 单个文件 ≤ 5MB，超过就降帧率或裁剪时长；
- 原始视频不在页面中引用。

---

## 7. 仓库门面

### 7.1 预编译 Release

- 产物：`proJV-<version>-windows-x64.zip`、`proJV-<version>-linux-x64.tar.gz`。
- **复用现有打包规则，不重写**：`CMakeLists.txt:245-303` 已有 `install()`、依赖产物清理和 CPack。包内容为 `proJV_tui` + `projv_files/{fonts,pytool,theme,skills}` + `README.md`、`README_zh.md`、`LICENSE`。
  - `prompts/`、`sessions/`、`config.toml` 由程序首次启动时生成（`CMakeLists.txt:254` 注释），不随包发布。
- **需要修改的地方**：
  - `CPACK_SYSTEM_NAME`、`CPACK_PACKAGE_FILE_NAME` 写死了 `win64`，`CPACK_GENERATOR` 只有 ZIP（`CMakeLists.txt:292-301`）。改为按平台区分：Windows 用 ZIP + `windows-x64`，Linux 用 TGZ + `linux-x64`。
  - `install(DIRECTORY projv_files/)`（`:258`）会打包整个目录。CI 是干净检出，不受影响；但要在注释里提醒：本地打包前确认目录里没有 `config.toml` 和 `sessions/`，否则会带出 API Key 和对话记录。最好在 `install` 中显式 `EXCLUDE` 这两项。
- 由 `.github/workflows/release.yml` 在打 tag `v*` 时自动构建、`cpack` 打包、上传。
- 发布前在干净的 Windows / Linux 虚拟机上，从下载到首次对话完整走一遍，作为 Release 验收。
- Release 正文沿用 `RELEASE_NOTES_v*.md` 的格式。
- **Windows**：二进制未签名，首次运行可能被 SmartScreen / Defender 拦截。处理方法写进 Release 正文和 `01-install.md`。
- **Linux**：libcurl 用系统库。Release 正文写明需要安装的依赖包；执行时用 `ldd` 确认是否还依赖其他系统库。

### 7.2 README 对比表

- 对比对象：2~3 个常见终端 AI 工具（例如 Claude Code、Aider、Codex CLI）。
- 对比维度：只选 proJV 有优势的维度——安装体积与依赖、原生性能/运行时、数据驱动的多 Agent 机制、扩展方式、使用成本，避免把弱项摆进表格。
- 规则：
  - 只写能公开查证的事实，表下注明查证日期；
  - 不确定的格子写"—"，不推测；
  - 语气中性，只陈述差异，不贬低其他工具；
  - proJV 一列以代码为准。

### 7.3 GitHub 仓库设置

这一项需要用户在 GitHub 网页上操作。执行 Agent 负责产出可直接粘贴的内容：
- **Description**：中英各一句，≤ 120 字符。
- **Topics**：例如 `ai-agent`、`tui`、`cpp`、`cpp20`、`deepseek`、`terminal`、`multi-agent`、`ftxui`、`llm`。
- **Social preview**：`assets/social-preview.png`，1280×640，由 Logo + 一句话定位组成；用户在 Settings → Social preview 上传。
- **Website**：如无官网，留空或填 Releases 页链接。

### 7.4 CI 与徽章

- `.github/workflows/ci.yml`：push 和 PR 触发，矩阵为 `windows-latest` + `ubuntu-latest`。
  - 构建：`cmake -B build -DCMAKE_BUILD_TYPE=Release` + `cmake --build build --config Release`。测试默认随构建（`PROJV_BUILD_TESTS` 默认 ON）。
  - 测试：`ctest --test-dir build -C Release --output-on-failure`（已注册为 `proJV_tests`，`tests/CMakeLists.txt:70`）。
  - Linux job 先安装依赖：`sudo apt-get install -y libcurl4-openssl-dev`（`find_package(CURL REQUIRED)`，`CMakeLists.txt:74`）。
- **已核实，可以直接上 CI**：
  - 单测不联网：`test_web_api.cpp` 只测格式化函数；所有技能测试都给每个 agent 设了 responder；
  - golden 测试用 `Screen::Create` 在内存渲染，不需要终端；
  - 示例技能走源码目录（`PROJV_EXAMPLE_SKILLS_DIR`），其余测试在 exe 目录下自建临时目录。
- **前置修复（1 行）**：`tests/CMakeLists.txt:66` 的 `add_compile_options(/utf-8)` 写在 `add_executable` 之后，对 `proJV_tests` 不生效。改为 `target_compile_options(proJV_tests PRIVATE /utf-8)`。否则英文代码页的 Windows runner 会把测试里的中文字面量（如 `test_markdown_render.cpp:91`）编错。
- **Linux 未曾验证**：仓库里没有近期 Linux 构建的证据，第一次 CI 运行即作为验证。失败就单独修，不阻塞 Windows job。
- Windows runner 上 `external/curl-8.21.0` 从源码编译较慢，可用 `actions/cache` 缓存构建目录。
- README 徽章：CI 状态、最新 Release 版本、License。

### 7.5 社区文件

- **`CONTRIBUTING.md` / `CONTRIBUTING_zh.md`**（路标式，细节链接到 dev-guide）：
  - 开发环境与构建测试命令；
  - 先读哪几页 dev-guide；
  - 提交信息和分支约定；
  - PR 要求：测试通过、改动涉及文档时中英一起更新。
- **`.github/ISSUE_TEMPLATE/bug_report.md`**：proJV 版本、操作系统、终端、模型、复现步骤、预期与实际、日志（执行时确认日志文件位置并写进模板）。
- **`.github/ISSUE_TEMPLATE/feature_request.md`**：场景、期望行为、可选方案。
- **`.github/PULL_REQUEST_TEMPLATE.md`**：改了什么、关联 issue、测试情况、中英文档是否同步的勾选项。

---

## 8. 执行顺序

| 阶段 | 内容 | 说明 |
|------|------|------|
| D1 | 现状核实 | 逐条核对现有 README 描述与 05 落地后的代码，列出过时项（例如思考链写着 deepseek-reasoner、主题数量前后不一致、`projv_files/prompts/` 首次运行才生成、工具列表新增 `list_dir`） |
| G1 | CI + Release 流水线 | 7.1、7.4。先做 `/utf-8` 修复和 CPack 平台区分，再写 workflow。README 的"3 步上手"依赖下载链接，所以排在 D4 之前；可与 D2 并行 |
| D2 | 开发指南 | 先写 `module-map.md`，需要逐个模块读代码 |
| D3 | 用户指南 | 可与 D2 并行，图片先占位 |
| D4 | README 改版 | 含对比表（7.2）和徽章。最后做，因为要链接到 D2、D3 的页面和 G1 的 Release |
| G2 | 社区文件 | 7.5，CONTRIBUTING 链接到 D2 的页面 |
| G3 | 仓库设置文案 | 7.3，产出文案和 social preview 图，交给用户在网页上操作 |
| D5 | 素材 | 用户录制视频后，转换 GIF 并替换占位 |

每个阶段中英两份一起完成。

---

## 9. 待用户提供 / 操作

1. Logo 文件的路径和格式（D4、G3 之前）。
2. `assets/raw/` 下的录制视频（D5 之前）。
3. 打第一个版本 tag，触发 Release 流水线（G1 完成后）。
4. 在 GitHub 网页上填写 Description、Topics，上传 Social preview（G3 完成后）。


---

## 10. 执行记录（v0.6.0）

### D1 现状核实 —— 发现并已修掉的过时/错误项

| 位置 | 问题 | 处理 |
|---|---|---|
| README 主题段 | "Two built-in presets ... plus 6 curated" 与别处"8 presets"互相矛盾 | 统一为「2 内置 + 6 套 JSON」 |
| README 功能表 | 思考链写成 `deepseek-reasoner`（旧模型名，且思考已是模型级开关） | 改为「thinking 流式显示」 |
| README 功能表 | 工具列表缺 `list_dir`、`format_file`，把 pytool 的 `diagram_tool` 混在内置工具里 | 重写为内置工具表 + 单独说明 `diagram_tool` 是示例 pytool |
| README 技能段 | monica 仍描述为 formatter / translator / auditor 三角色 | 改为 1 主管 + 10 worker |
| README 命令段 | 未提 `/workspace`；旧 `/skill <名字>` 形式其实已废弃 | 统一为 `/skill list` + `/<技能> <主题>`，并补上 `/workspace` |
| README 目录树 | `projv_files/prompts/` 被写成「已有」 | 标注为首次运行生成、不随包发布 |
| `tests/CMakeLists.txt:66` | `/utf-8` 写在 `add_executable` 之后，对测试无效 | 在 P1 修掉（`target_compile_options`） |

### 已完成

- **G1**：`project VERSION` 0.5.6 -> 0.6.0；CPack 按平台区分（Windows ZIP +
  `windows-x64`，Linux TGZ + `linux-x64`），包名 `proJV-<ver>-<platform>`；
  `install()` 显式排除 `config.toml` / `sessions` / `prompts`；`.github/workflows/`
  新增 `ci.yml`、`release.yml`。本地 `cpack` 已产出并核对包内容（无 Key、无会话）。
- **D2**：`docs/{en,zh}/dev-guide/`（README、module-map、recipes），中英镜像。
- **D3**：`docs/{en,zh}/user-guide/` 01~08 共 18 页，中英镜像，统一页尾「想深入 / 想改它」。
- **D4**：`README.md` / `README_zh.md` 重写：徽章、3 步上手、功能表、对比表（事实
  核实日 2026-10-25）、三个入口、理念、致谢。快捷键表与构建细节已移出到用户指南。
- **G2**：`CONTRIBUTING.md` / `CONTRIBUTING_zh.md`、两个 issue 模板、PR 模板。
- **G3**：`docs/repo-settings.md` 给出 Description（中英）、Topics、Social preview
  说明、首个 Release 步骤、分支保护建议。
- Logo 从仓库根移到 `assets/logo.jpg`；`assets/README.md` 与 `assets/raw/README.md`
  写明 6 段录屏的规格与转换命令（占位清单已列）。

### 待用户操作

1. `assets/raw/` 下的 6 段录屏；我据此转出 `assets/*.gif` 替换占位。
2. `assets/social-preview.png`（1280x640）——需要 Logo 定稿后生成。
3. 在 GitHub 网页填 Description / Topics / Social preview（文案见
   `docs/repo-settings.md`）。
4. 打第一个 tag `v0.6.0` 触发 Release 流水线。
5. Linux 构建与首次 CI 运行结果需要在 GitHub 上确认（本地未验证）。
