# proJV 图像理解支持 -- 执行方案

## 目标

`deepseek-flash` 支持本地图片输入。用户在消息里写 `@<路径>` 附上图，Agent 自动读取、base64 内联发送。

**关键约束（据官方文档）**：
- 图片仅允许出现在 `user` 消息，否则 400。
- 格式 JPEG/PNG/GIF/WebP（按文件内容判断）。
- 请求体 ≤ 48 MiB，单图 ≤ 32 MiB，单边 ≤ 8192 px。
- 图片 token 估算按每张上限 1024。
- 保持零新三方依赖，`deepseek.cpp` 请求构建零改动（序列化统一走 `models.cpp::to_json`）。

## 语法规范

```
@<路径>
```

| 输入 | 行为 |
|------|------|
| `@<文件>` | 单图 |
| `@<目录>` | 自动枚举目录内图片，全部内联发送（不弹确认、不弹列表） |
| 路径含空格 | 用引号包裹：`@"C:\my folder\a.png"` |

- **detail 固定为 `auto`**（Phase 1 不暴露选项）。
- **目录展开上限 8 张**（详见"多个 `@` 合并规则"）。

### 触发判定（防误伤普通文本）

`@` 后必须是以下形态之一才视为图片标记，否则按普通文本透传：
- 引号包裹：`@"<path>"`
- 盘符/根路径开头：`@C:\...`、`@/...`、`@./...`、`@..\...`

邮箱等（如 `a@b.com`）不满足上述形态，不会被误解析。

### 多个 `@` 合并规则（同一消息）

- 按书写顺序解析，文件与目录混用按顺序合并成**单一图片列表**。
- **8 张为整个消息的全局上限**（不是每个目录各 8 张）；合并后总 base64 体积 ≤ 44 MiB；单图 ≤ 30 MiB。任一超限 → 报错拒绝发送。
- 顺序确定性：目录内部按文件名排序后并入列表。

### 目录展开规则

只扫顶层不递归；魔数过滤（JPEG/PNG/GIF/WebP，静默跳过其他文件）；文件名排序保证顺序稳定。

### 多轮对话

每轮消息独立携带各自的 `images`，互不合并；历史重放时（Phase 2 持久化）各轮按消息还原。

### 与 skill 的交互（暂不支持共存）

`!<技能名>` 触发 skill 时，同一消息中的 `@<路径>` 图像**忽略并提示**（系统气泡：图像已忽略，skill 暂不支持携带图像），不静默丢弃。

## 实施步骤

### Phase 1：核心回路

1. **`models.h`**：新增 `modelSupportsVision(id)`（`id.find("flash") != npos`）+ `ImageAttachment{path, detail}` + `Message::images`。
2. **`tools/base64.h`**（新建）：标准 base64 编码 + 魔数嗅探（JPEG/PNG/GIF/WebP）。
3. **`models.cpp`**：`to_json` 对带图 user 消息输出 OpenAI content 块数组（text + N 个 image_url data URL），无图消息保持字符串不变，非 user 角色丢弃 images；`from_json` 恢复 `images`。
4. **`app_tui.cpp`**：`sendMessage()` 解析 `@<路径>` → 文件/目录展开 → 校验（存在/魔数/单图 ≤ 30 MiB/总量 ≤ 44 MiB/数量 ≤ 8）→ 从文本剥离标记 → 组装 Message 发送；模型不支持或超限时系统气泡报错，不发送。
5. **`bubble_model.cpp`**：带图用户气泡渲染 `[Image] <filename>` 列表（发送后记录，非发送前确认）。

### Phase 2：完整性与健壮性

6. **`storage.cpp/.h`**：`messages` 表加 `image_paths` 列（JSON 数组存路径），ALTER 容错兼容旧库；`insertMessage`/`queryMessages` 读写。
7. **`session.cpp`**：`estimateTokens` 每张图 +1024（low ~400），纳入上下文压力计算。
8. **`agent.cpp`**：HTTP 400 错误文案提示「当前模型可能不支持图片输入」。
9. **compactor 补偿（`session.cpp` + `prompts/compactor.md`）**：
  - 目的：带图消息在 LLM 压缩时图像上下文会丢失；DB 存的是路径，图本身不丢，但压缩摘要需保留"该消息含图"的事实。
  - 改法：`buildCompactionInput` 对带图消息的文本部分附加标记 `[含 N 张图]`；`compactor.md` prompt 加一句"遇到 [含 N 张图] 标记时，在摘要中保留该事实"。
  - 注意：压缩后仍可经 `to_json` 按路径重读图，无永久损坏；`from_json`/`applyCompaction` 对 `images` 字段的处理在实施时确认一次。

### Phase 3：测试

9. 单测：多模态 `to_json` 黄金 JSON（无图消息不变）、`@` 解析（引号路径/多图/目录展开/非法报错）、base64 往返、魔数嗅探、token 估算。
10. `clean_and_build.bat` 全量构建回归。