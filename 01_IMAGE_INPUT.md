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

`/<技能名>` 触发 skill 时，同一消息中的 `@<路径>` 图像**忽略并提示**（系统气泡：图像已忽略，skill 暂不支持携带图像），不静默丢弃。

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
---

## 实施记录（v0.5.7）

状态：**Phase 1 / 2 / 3 全部完成**。提交 `1b9db1f`。

### 代码落点

| 文件 | 内容 |
|---|---|
| `src/tools/base64.h` | 新增。标准 base64 + 魔数嗅探（JPEG/PNG/GIF/WebP）+ `makeImageDataUrl()` |
| `src/core/image_input.h/.cpp` | 新增。`@<path>` 解析、目录展开、三重上限校验、文本剥离。放在 core 而不是 `app_tui.cpp`，纯粹为了可单测 |
| `src/client/models.h/.cpp` | `ImageAttachment`、`Message::images`、`modelSupportsVision()`；`to_json` 输出 content 块数组，`from_json` 还原 |
| `src/tui/app_tui.cpp` | `sendMessage()` 的 `@` 解析分支（排在技能分支之后）；`startTurn(text, images)` |
| `src/tui/bubble_model.cpp` | 用户气泡追加 `[Image] <filename>` 行 |
| `src/core/storage.cpp` | `image_paths` 列（CREATE + ALTER 容错 + COALESCE 读取），JSON 数组只存路径 |
| `src/core/session.h/.cpp` | `estimateImageTokens()`（1024/张）；`buildCompactionInput` 追加 `[+N images]` |
| `src/core/prompts_loader.cpp` | `compactor.md` 默认加一句 `[+N images]` 说明，并对旧种子做幂等迁移 |
| `src/core/agent.cpp` | HTTP 400 且会话含图时追加"当前模型可能不支持图像输入"提示 |
| `tests/unit/test_image_input.cpp` | 16 组：base64 往返、魔数、各种 `@` 形态、拒绝分支、序列化、token、模型判定 |

### 实施决策（文档留白处，均已可改）

1. **纯图片消息的文本**：剥离标记后若正文为空，补 `"Describe the attached image(s)."`，保证 content 块里始终有一个非空 text 块。
2. **空格处理**：剥离标记后把连续空格/制表符折叠为一个（不动换行），否则 `a @"x" b` 会留下双空格。
3. **路径规整**：`@./a.png` 经 `lexically_normal()` 归一，与 `@a.png` 指向同一路径（Windows 上 C++20 的 `u8string()` 返回 `std::u8string`，已显式转换）。
4. **`ImageAttachment` 多两个字段**：`mime` 与 `dataUrl` 缓存。文档只写了 `{path, detail}`；缓存让同一请求不用每次重新读盘 + base64。`dataUrl` 不落库，DB 只存 path，`to_json` 发现缓存为空时按 path 重读。
5. **不可读文件不致命**：序列化时若某张图丢失，退化成 `[image unavailable: <path>]` 文本块而不是让整个请求 400。
6. **`detail` 恒为 `auto`**（文档要求），字段已预留。

### 待你确认的一处语义（当前取舍已实现）

文档同时写了"压缩需保留该消息含图的事实"与"压缩后仍可按路径重读图、无永久损坏"。两句话只部分成立：

- 事实保留：已实现，`[+N images]` 标记进入压缩输入，并写了对应 prompt 指令。
- 图像本身：一旦该消息被 LLM 压缩替换为摘要，**图像就从活动上下文消失**（路径仍在 DB，重新打开会话会从 DB 还原并重放）。
- 若要"压缩后仍能重放图像"，需要把带图消息列入 `planCompaction` 的 pinned 集合（当前未做）。

### 验证情况

- 单测：16 组全通过（含目录展开排序、非图片静默跳过、超 8 张/缺文件/非图片格式的拒绝分支、`to_json` 黄金结构）。
- 启动验证：清空 `projv_files/prompts` 后启动，`coder.md` 正确带一个 `run_skill`；把工具行回退成 0.5.6 的旧值再启动，迁移只补一次（此前发现并修复了"补两次"的 bug，已加 `test_prompt_upgrade.cpp` 锁死）。
- **真机未验证**：真实 API 是否接受 content 块数组（官方文档给的结构与实现一致），以及真实图片的 token 计费。原因同 03：仓库里的 Key 返回 401。
