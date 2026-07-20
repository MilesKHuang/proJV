# proJV 主题颜色系统设计

**版本**: 1.0  
**日期**: 2025-07-15  
**状态**: Design Phase

---

## 1. Background & Goals

### 1.1 Current State

当前项目中颜色分散在 5 个文件中，全部硬编码 `ImVec4(...)`：

| 文件 | 颜色数量 | 用途 |
|---|---|---|
| `src/ui/app.cpp` (`applyTheme()`) | 8 | ImGui 全局 style 色（WindowBg、MenuBarBg、Text 等） |
| `src/ui/render_chat.cpp` | ~35 | 聊天气泡背景、工具调用颜色、Agent 阶段色、状态指示器 |
| `src/ui/markdown_render.cpp` | 14 | Markdown 渲染 (H1/H2/H3, bold, italic, code, link, quote 等) |
| `src/ui/render_settings.cpp` | ~6 | 工具审批弹窗文本颜色 |
| `src/main.cpp` | 1 处 | D3D clear color `(0.08, 0.08, 0.10)` |

**Pain points:**
- 修改一个颜色需要在多个文件中定位对应的 `ImVec4`
- 所有颜色耦合在渲染代码中，无法做全局切换
- 无持久化 — 重启 APP 丢失用户偏好
- 仅支持 2 个硬编码主题（Dark / GitHub Dark），且区别极小

### 1.2 Goals

1. **集中定义**: 所有项目颜色统一在 `ThemeColors` 结构体中，单一头文件 `src/ui/theme.h`
2. **主题弹窗**: 工具栏 Settings → Theme → "Customize..." 打开一个弹窗，列出所有颜色槽位，每个槽位支持 `#RRGGBB` 输入
3. **实时预览**: 弹窗右侧有一个 Mini 预览面板，展示几个关键组件（聊天气泡、Markdown 标题、代码块）的搭配效果
4. **JSON 导出与自动加载**: 可导出主题为 JSON 到 `projv_theme/` 目录；启动时自动扫描 `projv_theme/` 下所有 `.json`，若存在则自动加载第一个有效主题

### 1.3 Design Principles

- **零侵入对 ImGui Style**: 主题既驱动 `ImGui::GetStyle().Colors[]` 也驱动自定义绘制色
- **Hex string 作为输入/输出格式**: `#RRGGBB` 对用户直观，JSON 中也用 hex string
- **最小改动现有代码**: 使用全局单例 + 查询函数，渲染代码中 `ImVec4(...)` 替换为 `theme.xxx()`
- **与现有 config.toml 分离**: 主题独立于 API 配置，存储在 `projv_theme/` 目录

---

## 2. Architecture

### 2.1 Current Architecture

```
main.cpp
  ├── ImGui::StyleColorsDark()           ← 初始 ImGui 风格
  └── g_app.render()
        └── app.cpp
              ├── applyTheme(0/1)         ← 8 个 ImGuiCol_* 覆盖
              ├── renderChatArea()        ← ~30 个硬编码 ImVec4
              │     └── markdown_render.cpp  ← 14 个 static const ImVec4
              ├── renderInputArea()       ← spinner 颜色
              ├── renderStatusBar()       ← ~15 个 TextColored 颜色
              ├── renderWelcomePage()     ← 2 个颜色
              └── renderTodoPanel()       ← 6 个颜色
```

### 2.2 Proposed Architecture

```
main.cpp
  ├── ImGui::StyleColorsDark()           ← 初始 fallback
  ├── ThemeManager::init()               ← [NEW] 自动检测 & 加载主题
  ├── ThemeManager::applyToImGui()       ← [NEW] 覆盖 ImGui Style
  └── g_app.render()
        ├── app.cpp
        │     ├── applyTheme()           ← [MODIFIED] 调用 ThemeManager
        │     ├── renderMainMenuBar()    ← [MODIFIED] 新增 Theme 子菜单入口
        │     ├── renderChatArea()       ← [MODIFIED] ImVec4 → theme.xxx
        │     ├── renderInputArea()      ← [MODIFIED] ImVec4 → theme.xxx
        │     ├── renderStatusBar()      ← [MODIFIED] ImVec4 → theme.xxx
        │     ├── renderWelcomePage()    ← [MODIFIED] ImVec4 → theme.xxx
        │     └── renderTodoPanel()      ← [MODIFIED] ImVec4 → theme.xxx
        ├── render_chat.cpp              ← [MODIFIED] 硬编码 → theme ref
        ├── render_settings.cpp          ← [MODIFIED] 硬编码 → theme ref
        ├── markdown_render.cpp          ← [MODIFIED] static const → theme ref
        └── theme_popup.cpp              ← [NEW] 主题编辑弹窗 + 预览
src/ui/theme.h                           ← [NEW] ThemeColors 结构体 + ThemeManager
src/ui/theme.cpp                         ← [NEW] 实现: init, save, load, apply, export
```

### 2.3 Key Differences

| Component | Current | Proposed |
|---|---|---|
| 颜色定义位置 | 分散在 6 个 .cpp 文件中 | `theme.h` 单一结构体 `ThemeColors` |
| ImGui Style | `applyTheme()` 硬编码 8 色 | `ThemeColors::applyToImGui()` 覆盖所有相关 ImGuiCol_* |
| Markdown 颜色 | 14 个文件级 `static const ImVec4` | `ThemeColors` 成员字段，通过引用读取 |
| 用户自定义 | 不支持 | Hex 输入弹窗 + 实时预览 + JSON 导出 |
| 持久化 | 无 | JSON 文件 + 启动自动扫描 |
| 主题切换 | 2 个硬编码 index | 动态列表（内置 + 用户安装） |

## 3. Core Design

### 3.1 ThemeColors 结构体

所有颜色归类为明确的语义组，每个字段是一个 hex string (`std::string`)，提供 `toVec4()` / `toU32()` 转换。

```cpp
// src/ui/theme.h

#pragma once
#include <imgui.h>
#include <string>
#include <vector>
#include <cstdint>

struct ThemeColors {
    // ---- Metadata ----
    std::string name = "Default Dark";
    std::string author = "proJV";
    std::string version = "1.0";

    // ---- ImGui Global Style (映射到 ImGuiCol_*) ----
    std::string windowBg       = "#1E1E24";
    std::string menuBarBg      = "#17171C";
    std::string frameBg        = "#2E2E33";
    std::string text           = "#EBEBF2";
    std::string textDisabled   = "#80808C";
    std::string titleBg        = "#17171C";
    std::string scrollbarBg    = "#17171C";
    std::string scrollbarGrab  = "#4D4D59";

    // ---- Chat Bubbles ----
    std::string bubbleUserBg      = "#5F7B8C";
    std::string bubbleAssistantBg = "#6B8E6B";
    std::string bubbleSystemBg    = "#4D4D59";
    std::string bubbleDefaultBg   = "#38383D";
    std::string bubbleCompactedBg = "#594714";

    // ---- Tool Call Bubbles ----
    std::string toolTitleColor     = "#FF9926";
    std::string toolBg             = "#474752";
    std::string toolResultText     = "#B3B3BF";

    // ---- Reasoning (Chain-of-Thought) Card ----
    std::string reasoningCardBg     = "#1A0F29";
    std::string reasoningBorder     = "#59338C";
    std::string reasoningTextColor  = "#B89EE0";
    std::string reasoningBodyBg     = "#120A1F";
    std::string reasoningBodyText   = "#AD94D9";

    // ---- Copy Button ----
    std::string copyBtnBg      = "#2E2E38";
    std::string copyBtnHovered = "#404050";

    // ---- Agent Phase Indicators ----
    std::string phaseIdle           = "#808099";
    std::string phaseStreaming      = "#FFE64D";
    std::string phaseExecutingTools = "#4DD9A6";
    std::string phaseAwaitApproval  = "#FF5959";
    std::string phaseError          = "#FF3333";

    // ---- Status Bar ----
    std::string statusTokenInfo   = "#808099";
    std::string statusCtxHigh     = "#E6801A";
    std::string statusCtxCritical = "#FF3333";
    std::string statusCtxMedium   = "#CCCC33";
    std::string statusCtxLow      = "#4DB34D";
    std::string statusRunning     = "#FF9926";
    std::string statusError       = "#FF3333";
    std::string statusIdle        = "#808099";

    // ---- TODO Panel ----
    std::string todoTitle      = "#FFD959";
    std::string todoDone       = "#4DD966";
    std::string todoInProgress = "#FFCC4D";

    // ---- Markdown ----
    std::string mdH1        = "#FFB847";
    std::string mdH2        = "#FFA659";
    std::string mdH3        = "#E6CC80";
    std::string mdBold      = "#FFD159";
    std::string mdItalic    = "#8CCCF2";
    std::string mdCode      = "#E69973";
    std::string mdLink      = "#66A6FF";
    std::string mdLinkUnder = "#4D80D9";
    std::string mdBullet    = "#80B3E6";
    std::string mdHR        = "#4D5261";
    std::string mdCodeBg    = "#242933";
    std::string mdQuote     = "#8C8C99";
    std::string mdQuoteBar  = "#668CB3";
    std::string mdTableHdr  = "#383D4D";

    // ---- D3D Clear Color ----
    std::string clearColor = "#14141A";

    // ---- Converters ----
    ImVec4 toVec4(const std::string& hex) const;
    ImU32  toU32 (const std::string& hex) const;

    // ---- Bulk apply ----
    void applyToImGui() const;

    // ---- Serialization ----
    std::string toJson() const;
    static ThemeColors fromJson(const std::string& json);
    static ThemeColors defaultDark();
};
```

**Rationale:**
- 使用 `std::string` hex 而非 `ImVec4`：hex 是人类可读通用格式，JSON 原生支持
- `toVec4()` / `toU32()` 转换开销极小（每帧 ~60 次 hex→float，远低于 16.7ms 预算）
- 分组命名清晰（`bubble`, `tool`, `reasoning`, `phase` 等）

### 3.2 ThemeManager 单例

```cpp
// src/ui/theme.h (续)

class ThemeManager {
public:
    static ThemeManager& instance();

    void init(const std::string& exeDir);
    const ThemeColors& current() const { return current_; }
    bool switchTo(const std::string& name);
    void applyCustom(const ThemeColors& tc);
    bool exportToFile(const std::string& filename) const;
    const std::vector<std::string>& installedThemes() const { return installedNames_; }
    const std::string& themeDir() const { return themeDir_; }

private:
    ThemeManager() = default;
    ThemeColors current_;
    std::string themeDir_;
    std::vector<std::string> installedNames_;
    std::vector<ThemeColors> installedThemes_;
    void scanThemeDir();
    ThemeColors loadBuiltin(const std::string& name);
};
```

**关键行为:**
- `init()`: 先设 `current_` 为 `defaultDark()`; 扫描 `projv_theme/`，若存在 `.json` 则加载第一个有效的覆盖 `current_`; 最后调用 `current_.applyToImGui()`
- `switchTo()`: 先在内置列表中查找，再在已安装列表中查找
- `exportToFile()`: 序列化 `current_` 为 JSON，写入 `projv_theme/<filename>.json`

### 3.3 主题编辑弹窗

在 `renderMainMenuBar()` 的 Settings 菜单增加 "Theme" 子菜单:

```
Settings → Theme
  ├── Default Dark        ← 内置，radio 选择
  ├── GitHub Dark
  ├── ─────────────
  ├── my-custom           ← 已安装主题（动态）
  ├── ─────────────
  └── Customize...        ← 打开编辑弹窗
```

弹窗布局 (`theme_popup.cpp`):

```
┌─────────────────────────────────────────────────────┐
│  Theme Editor                          [Export JSON]│
├──────────────────────┬──────────────────────────────┤
│ Category: [Chat ▾]   │                              │
│                      │   ┌─────────────────────┐    │
│ bubbleUserBg         │   │  User: Hello!       │    │
│  [#5F7B8C]  [████]   │   │  ┌─────────────────┐│    │
│                      │   │  │ Assistant: Hi!   ││    │
│ bubbleAssistantBg    │   │  │ ┌───────────────┐││    │
│  [#6B8E6B]  [████]   │   │  │ │ ## Markdown   │││    │
│ ...                  │   │  │ │ **bold**       │││    │
│                      │   │  │ │ `code`         │││    │
│                      │   │  │ └───────────────┘││    │
│                      │   │  └─────────────────┘│    │
│                      │   └─────────────────────┘    │
├──────────────────────┴──────────────────────────────┤
│                          [Apply]  [Save As...]  [X] │
└─────────────────────────────────────────────────────┘
```

**实现细节:**
- 左侧: ComboBox 选择颜色分类（Chat/Tool/Reasoning/Phase/Status/TODO/Markdown/Style），下面列出该分类所有字段名 + hex 输入框 + 小色块
- 右侧: `BeginChild` 区域，用当前编辑中的颜色渲染 Mini 预览面板：User 气泡 + Assistant 气泡（含 Markdown 标题/粗体/代码）+ 工具调用气泡
- hex 输入框改变时实时更新预览（维护一个本地 `ThemeColors editCopy_`）
- "Apply" 按钮: 调用 `ThemeManager::applyCustom(editCopy_)` 立即生效
- "Save As...": 弹出保存对话框，默认路径 `projv_theme/`
- "Export JSON": 快速导出到 `projv_theme/` 下自动命名文件

### 3.4 JSON 格式

```json
{
  "name": "My Solarized",
  "author": "user",
  "version": "1.0",
  "colors": {
    "windowBg":       "#002B36",
    "menuBarBg":      "#073642",
    "bubbleUserBg":   "#268BD2",
    "bubbleAssistantBg": "#859900",
    "...": "..."
  }
}
```

`colors` 下的 key 名称与 `ThemeColors` 字段名严格一致，`fromJson()` 通过字段名映射赋值。缺失字段保持 `defaultDark()` 默认值。

### 3.5 自动加载逻辑

```cpp
void ThemeManager::init(const std::string& exeDir) {
    themeDir_ = exeDir + "/projv_theme";
    current_ = ThemeColors::defaultDark();   // fallback

    std::filesystem::create_directories(themeDir_, ec);

    for (auto& entry : std::filesystem::directory_iterator(themeDir_)) {
        if (entry.path().extension() == ".json") {
            std::ifstream f(entry.path());
            std::string json((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
            try {
                auto tc = ThemeColors::fromJson(json);
                installedNames_.push_back(tc.name);
                installedThemes_.push_back(std::move(tc));
            } catch (...) { /* skip invalid */ }
        }
    }

    if (!installedThemes_.empty()) current_ = installedThemes_[0];
    current_.applyToImGui();
}
```

### 3.6 现有代码修改策略

每个使用主题色的 `.cpp` 文件顶部添加便捷引用:

```cpp
// render_chat.cpp 顶部
static const ThemeColors& T = ThemeManager::instance().current();

// 使用:
ImGui::PushStyleColor(ImGuiCol_ChildBg, T.toVec4(T.bubbleUserBg));
```

`markdown_render.cpp` 中 14 个 `static const ImVec4` 删除，改为在 `renderMarkdown()` 入口从 `ThemeManager` 获取颜色并转换为局部 `ImVec4` 变量。转换开销可忽略。

---

## 4. Changes List

| File | Action | Description | ~Lines |
|---|---|---|---|
| `src/ui/theme.h` | **New** | `ThemeColors` 结构体 + `ThemeManager` 单例声明 | 120 |
| `src/ui/theme.cpp` | **New** | 实现: `toVec4/toU32`, `applyToImGui`, `toJson/fromJson`, `init`, `switchTo`, `exportToFile`, `scanThemeDir` | 200 |
| `src/ui/theme_popup.cpp` | **New** | 主题编辑弹窗 + Mini 预览面板 | 250 |
| `src/main.cpp` | **Modified** | `ImGui::StyleColorsDark()` 之后调用 `ThemeManager::instance().init(exeDir)`; clearColor 从 theme 读取 | +8 |
| `src/ui/app.h` | **Modified** | 删除 `themeNames`、`selectedThemeIndex`; 添加 `showThemeEditor` flag | -3 / +3 |
| `src/ui/app.cpp` | **Modified** | `applyTheme()` 改为调用 `ThemeManager::switchTo()`; `renderMainMenuBar()` 增加 Theme 子菜单; `render()` 增加弹窗调用; 状态栏/欢迎页/TODO面板硬编码色替换 | ~40 |
| `src/ui/render_chat.cpp` | **Modified** | 所有硬编码 `ImVec4` 替换为 `T.toVec4(T.xxx)` | ~35 |
| `src/ui/render_settings.cpp` | **Modified** | 硬编码色替换为 theme 引用 | ~6 |
| `src/ui/markdown_render.cpp` | **Modified** | 删除 14 个 `static const ImVec4`; 从 `ThemeManager` 获取颜色 | ~20 |
| `CMakeLists.txt` | **Modified** | `COMMON_SOURCES` 增加 `theme.cpp` 和 `theme_popup.cpp` | +2 |

**总计**: 2 个新文件 (~570 行), 7 个修改文件 (~114 行修改)

---

## 5. Risks & Considerations

### 5.1 性能: hex → ImVec4 转换

**风险**: `ThemeColors` 中 ~50 个字段，每次使用调用 `toVec4()` 做 hex 解析。

**评估**: `toVec4()` 解析 6 个 hex 字符，约 10ns。50 字段 × 10ns = 500ns。ImGui 帧预算 16.7ms，占比约 0.003%。可忽略。

**缓解**: 若后续实测有问题，在 `applyCustom()` 时预计算所有 `ImVec4` 缓存到内部数组。当前不做。

### 5.2 线程安全

**风险**: `ThemeManager::instance()` 全局单例并发访问。

**评估**: 当前项目所有渲染和事件处理在 `WinMain` 主线程，不存在并发访问。无风险。

### 5.3 JSON 解析失败

**风险**: `projv_theme/` 下格式错误的 `.json` 导致启动 crash。

**缓解**: `fromJson()` 用 try/catch 包裹所有 `nlohmann::json` 操作；`scanThemeDir()` 跳过解析失败文件。保证 `current_` 始终 fallback 到 `defaultDark()`。

### 5.4 字段名不一致

**风险**: JSON `colors` key 与 `ThemeColors` 字段名不匹配导致颜色丢失。

**缓解**: `fromJson()` 对缺失字段保持 `defaultDark()` 默认值。`toJson()` 输出严格按字段名。新增字段时同步更新两个函数。

### 5.5 ImGui Style 冲突

**风险**: `applyToImGui()` 覆盖 `ImGui::GetStyle().Colors[]`，可能与 ImGui 内置行为冲突。

**缓解**: `applyToImGui()` 只覆盖 `ThemeColors` 中明确定义的 8 个 ImGuiCol_* 键，其余保持 `StyleColorsDark()` 默认值。用户自定义主题中若未提供这些字段也保持默认。
