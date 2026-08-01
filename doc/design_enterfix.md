# 输入框输入失灵修复

> 2026-07-31 | 设计阶段

---

## 1. 问题

| # | 现象 |
|---|------|
| 1 | Idle 状态下输入到一半突然失灵：无法输入字符、无法删除 |
| 2 | 需缩小窗口或做其他操作才能恢复输入 |
| 3 | Enter 键无法换行 |
| 4 | Ctrl+Enter 无法发送消息 |
| 5 | 发生频率极高，几乎每轮对话都会遇到 |

---

## 2. 根因

### 问题 1：`ImGui::Shortcut` 在 `InputTextMultiline` 之后调用，路由污染

**位置**：`render_chat.cpp` 第 692 行。

`ImGui::Shortcut()` 在内部注册全局快捷键路由状态，此状态跨帧残留。当前 `Shortcut` 在 `InputTextMultiline` 之后调用，路由状态污染下一帧的键盘事件分发，导致 `InputTextMultiline` 无法接收键盘输入。

**证据**：缩小窗口 → GLFW 焦点回调 → ImGui 重置路由状态 → 输入恢复。与 Shortcut 路由污染特征完全吻合。

### 问题 2：`InputTextMultiline` 无 `CtrlEnterForNewLine` 标志，且 Shortcut 键位不对

**位置**：`render_chat.cpp` 第 685 行 flags 为 `0`；第 692 行 Shortcut 为 `ImGuiMod_Ctrl | ImGuiKey_Enter`。

当前行为：Enter 被控件消费为换行，Shortcut 监听 Ctrl+Enter 发送。但这与直觉相悖——用户期望 Enter 直接发送。

**修正**：加 `ImGuiInputTextFlags_CtrlEnterForNewLine` 同时把 Shortcut 改为**纯 Enter**：
- 控件：Enter 不再产生换行，变为"提交"信号离开控件 → 被 Shortcut 捕获 → 发送
- 控件：Ctrl+Enter → 换行
- 这样 Enter 发送、Ctrl+Enter 换行，与主流聊天应用一致

### 问题 3：`BeginDisabled`/`EndDisabled` 内层冗余且自相矛盾

**位置**：`render_chat.cpp` 第 677/689 行 + `app.cpp` 第 494 行（外层）。

**代码现状**：

```cpp
// app.cpp render() L494
if (isWaiting) ImGui::BeginDisabled();   // [A] 外层 push ─┐
// ...                                                      │
// renderInputArea():                                       │
bool disableInput = isWaiting;          // 恒等于 isWaiting  │
if (disableInput) ImGui::BeginDisabled(); // [B] 内层 push ─┤ 双重嵌套
// InputTextMultiline (双重 disabled)                        │
if (disableInput) ImGui::EndDisabled();   // [C] 内层 pop ──┘ 提前解套
// Shortcut 区（单层 disabled by [A]）
ImGui::SameLine();
if (isWaiting) {
    ImGui::EndDisabled();                 // [D] pop [A] → Cancel 可点击
    Cancel button
    ImGui::BeginDisabled();               // [E] 恢复 [A]
}
// app.cpp 最后:
if (isWaiting) ImGui::EndDisabled();      // [F] pop [A]
```

**四个事实**：

1. `disableInput = isWaiting` — 两者**永远相等**，来自同一个 `agent->getStatus()`。
2. [B] 内层 push 完全多余 — app.cpp 已经 push 过一模一样的 disable。
3. [C] 内层 pop 的存在理由就是 [B] 多余：为了在 Shortcut 区不被双重 disable 卡死，不得不提前弹掉内层。一个 push 的存在意义是另一个 push 的副作用。
4. 真正的控制点只有**一个** — app.cpp 的 [A]/[F] 对。渲染输入区完全不需要自己管 disable，只需在 Cancel 按钮处借 [D]/[E] 挖个洞。

**修复**：删除内层 disable 整组逻辑（[B]/[C] 及关联的 `disableInput`/`wasDisabled`），仅保留 [D]/[E] 挖洞：

```cpp
// renderInputArea() —— 无内层 disable
InputTextMultiline   // Idle: 可编辑 / Waiting: 被 [A] 禁用
if (isIdle && Shortcut(ImGuiKey_Enter)) { ... }  // Shortcut 在前

ImGui::SameLine();
if (isWaiting) {
    ImGui::EndDisabled();   // pop 外层 [A] → Cancel 可点
    Cancel button
    ImGui::BeginDisabled(); // 恢复 [A]
} else {
    Send button
}
// EndChild; 回 app.cpp 由 [F] 收尾
```

栈路径清晰且只有一层，后续改动不可能出错。

### 问题 4：`static bool wasDisabled` 空逻辑（并入问题 3 一起删除）

**位置**：`render_chat.cpp` 第 680~681 行，if 体为空。与 `disableInput` 一起是内层 disable 的残留物，随问题 3 一并删除。

---

## 3. 修复方案

**仅修改 `src/ui/render_chat.cpp` 的 `App::renderInputArea()` 函数**。

### 3.1 修复问题 1：将 Shortcut 移到 InputTextMultiline 之前

**改动**：将第 692 行的 `ImGui::Shortcut` 块移到 `BeginChild("InputFill")` 之后、`InputTextMultiline` 之前。

### 3.2 修复问题 2：CtrlEnterForNewLine + EnterReturnsTrue + Shortcut 辅助

**关键认知**：仅加 `CtrlEnterForNewLine` 只是改变 Enter 语义（换行→提交），**控件有焦点时仍然消费 Enter 键**，Shortcut 拿不到这个事件。

**正确做法**：控件返回值 + Shortcut 双保险。

```cpp
// flags
ImGuiInputTextFlags_CtrlEnterForNewLine | ImGuiInputTextFlags_EnterReturnsTrue

// 发送触发
bool submitted = ImGui::InputTextMultiline("##input", ...);
if (isIdle && submitted) { /* 发送 */ }   // 控件有焦点：Enter 触发

// Shortcut 作为辅助（控件失焦时 Enter 也能发送）
if (isIdle && ImGui::Shortcut(ImGuiKey_Enter)) { /* 同逻辑 */ }
```

**标志分工**：
| 标志 | 作用 |
|------|------|
| `CtrlEnterForNewLine` | Enter = 提交（不产生换行），Ctrl+Enter = 换行 |
| `EnterReturnsTrue` | Enter 时 `InputTextMultiline` 返回 true |

**Shortcut 键位**：从 `ImGuiMod_Ctrl \| ImGuiKey_Enter` 改为 `ImGuiKey_Enter`（否则 Ctrl+Enter 被控件消费为换行后 Shortcut 永远收不到事件）。

**按钮提示**：Send 按钮下方加小字 `"Enter send  |  Ctrl+Enter newline"`。

### 3.3 修复问题 3：删除内层 disable，只保留外层

**核心认知**：`disableInput = isWaiting` 恒成立，来自同一个 `agent->getStatus()`。app.cpp 外层已 push 过一模一样的 disable，内层再 push 一次纯属冗余——这正是结构扭成麻花的根源。

**删除**：
- `bool disableInput = isWaiting;` — 恒等式
- `if (disableInput) ImGui::BeginDisabled();` — 内层 push，冗余
- `if (disableInput) ImGui::EndDisabled();` — 内层 pop，存在理由仅为前面的冗余 push
- `wasDisabled = disableInput;` / `static bool wasDisabled` 及空 if — 问题 4 一并解决

**不动**：`if (isWaiting) { ImGui::EndDisabled(); ... ImGui::BeginDisabled(); }` — 外层挖洞让 Cancel 可点，逻辑正确。

---

## 4. 改动清单

| # | 位置 | 操作 | 说明 |
|---|------|------|------|
| 1 | `bool disableInput = isWaiting;` | 删除 | 恒等式，内层 disable 冗余根源 |
| 2 | `if (disableInput) ImGui::BeginDisabled();` | 删除 | 内层 push，外层已 push 过 |
| 3 | `if (disableInput) ImGui::EndDisabled();` | 删除 | 内层 pop，仅为前者代价 |
| 4 | `static bool wasDisabled` + 空 if | 删除 | 问题 4，随问题 3 一并清除 |
| 5 | `wasDisabled = disableInput;` | 删除 | 同上 |
| 6 | InputTextMultiline flags `0` | 改 `ImGuiInputTextFlags_CtrlEnterForNewLine \| ImGuiInputTextFlags_EnterReturnsTrue` | Enter = 提交+返回 true，Ctrl+Enter = 换行 |
| 7 | 输入框后 | 新增 `bool submitted =` 接收返回值 | Enter 时有焦点走控件返回值，失焦走 Shortcut |
| 8 | Shortcut key `ImGuiMod_Ctrl \| ImGuiKey_Enter` | 改 `ImGuiKey_Enter` | 控件内 Ctrl+Enter 已被消费为换行，Shortcut 只监听纯 Enter |
| 9 | Shortcut 整块 | 移到 InputTextMultiline 之前 | 路由不再污染下一帧 |
| 10 | Send 按钮下方 | 新增小字提示 | "Enter send  \|  Ctrl+Enter newline" |

---

## 5. 验证方法

| 场景 | 预期行为 |
|------|----------|
| Idle 持续输入 | 不中断，可正常打字和删除 |
| Idle 状态 Enter | 发送消息 |
| Idle 状态 Ctrl+Enter | 换行 |
| Thinking 状态 | 输入框禁用，Cancel 可点击 |
| Thinking → Idle 切换 | 输入框立即恢复 |
| 连续多轮 | 每轮 Idle 后输入均正常 |

---

## 6. 风险

| 风险 | 影响 | 缓解 |
|------|------|------|
| 按钮提示仅在 Send 可见时显示 | Cancel 状态下不显示提示 | 可接受，Cancel 时用户不需要输入 |
| 修复后失灵仍存在 | 根因判断有误 | 在 `InputTextMultiline` 前后加 `debugLog` 打印 `IsItemFocused()`/`IsItemActive()`；或升级 ImGui 版本 |