# v0.5.0 信任与体验增强 设计方案

> ⚠️ 已废弃（2026-09-13）：v0.5.0 实际交付为 ImGui→FTXUI 迁移（见 `RELEASE_NOTES_v0.5.0.md`）。本设计（diff / snapshot / theme 个性化 / 文件树）未实现，保留作为未来参考。

> 版本: v1.1
> 日期: 2026-07-18
> 状态: 设计阶段

---

## 1. 背景与目标

### 1.1 现状

proJV v0.4.0 已具备完整的 designer/coder 双角色工作流。但存在四个短板：

1. **不可审查**：Agent 修改文件后，用户只能靠 `read_file` 肉眼对比，不知道改了哪里
2. **不可回退**：Agent 改崩了没有快照可回滚，只能手动 `git reset`
3. **不可个性化**：只有 Dark / GitHub Dark 两套主题，没有用户自定义空间
4. **文件结构不可见**：LLM 靠项目上下文文本猜测路径常猜错；用户没有直观的目录树

### 1.2 目标

1. 每次文件编辑后，在聊天区渲染可展开的 diff bubble
2. 每次 Agent 执行工具前自动 git snapshot，保证随时可回退
3. 支持用户自定义 accent color + 字体大小，提供几套经典预设
4. 侧边栏增加文件树 Tab，用 `ImGui::TreeNode` 渲染 workspace 目录结构

### 1.3 设计原则

- **最小侵入**：不改 Agent 状态机，不改 Session 结构
- **UI only**：diff、theme、文件树纯 UI 层；snapshot 纯 shell 调用
- **零外部依赖**：diff 生成、git snapshot、文件树遍历不引入新库


## 2. 架构

### 2.1 Diff Display

### 2.2 Snapshot

### 2.3 Theme Personalization

### 2.4 File Tree Sidebar