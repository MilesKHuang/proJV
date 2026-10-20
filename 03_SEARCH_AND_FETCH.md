# proJV 网页搜索与抓取集成 -- 执行方案

> 目标：DeepSeek 官方 Search / Fetch 接口（原始 JSON API，自研搜索引擎 + Markdown 抓取）第一时间集成，**无缝替换**现有 Bing 抓取式 `web_search` 与 libcurl 直抓式 `fetch_url`。

## 背景与目标

DeepSeek 推出官方 Search 与 Fetch 两个只读原始 JSON API，无需独立 Key、按次计费（5 元/千次，仅 200 且产生结果计费）。proJV 现有网页工具（`web_tools.cpp` 的 Bing HTML 抓取与 libcurl 直抓）应替换为官方接口。

**关键决策：保留工具名 `web_search` / `fetch_url` 不变，只替换 executor 内部实现** —— prompt 声明、`allowedTools`、会话历史全部不受影响，改动面最小、可第一时间上线。`DeepSeekClient` 负责鉴权与 HTTP，新增两个方法复用既有 libcurl 基础设施，零新依赖。

## 官方 API 事实（三份文档核对）

| 项 | Search | Fetch |
|----|--------|-------|
| Endpoint | `POST /v1/tools/search` | `POST /v1/tools/fetch` |
| Base URL | `https://api.deepseek.com`（复用 proJV `config.baseUrl`） | 同左 |
| 鉴权 | `Authorization: Bearer <key>` + `Content-Type: application/json` | 同左 |
| 请求字段 | `query`（必填，1~128 字符）、`count`（1~50，默认 10）、`ori_question`（1~128，助解意图）、`localization`（IP 本地化）、`need_content`（bool，默认 false，false 时 content 恒空） | `url`（必填，≤8192，含 host 的 HTTP/HTTPS，不带用户名/密码） |
| 响应 | `results[]`：`rerank_score`（0~3，≥1.5 优质）、`title`、`url`、`snippet`（100~200 字符）、`summary`（600~800 字符，适配模型）、`content`（正文，need_content=false 为空）、`publish_time`、`authority`（0~4，≥3 极权威） | `title`、`content`（**Markdown 正文**） |
| 搜索语法 | `site:`/`-site:`/`-`/`-"..."`/`"..."`/`after:`/`before:` 原样透传 | - |
| 空结果 | 200 + 空 `results[]` | 404（无法获取可用内容） |
| 计费 | 5 元/千次成功请求，仅 200 且 results 非空 | 5 元/千次成功请求，仅返回正文 |
| QPS | 100（账号级） | 25（账号级 + 目标域名级） |
| 可重试错误 | 408、429、5xx | 同左 |
| 不可重试错误 | 400/401/402/403/404/413（如不修改请求勿重试）；402 需充值 | 同左 |

> 提示：2026 年 11 月 1 日 00:00 起，在 Claude Code/Codex/Harness 等工具中的 Server 端 Search/Fetch 调用将从免费转为按次计费。proJV 作为自研集成，需从上线日起就按次计费意识设计（工具描述写清"按次收费，仅在需要实时信息时调用"）。

## 现有实现与官方接口比对

| 现状（`web_tools.cpp`） | 官方 Search/Fetch | 替换收益 |
|------|------|------|
| `web_search`：GET `bing.com/search?q=` + `<li class="b_algo">` HTML 块解析，最多 5 条，无 URL 编码兜底 | 结构化 `results[]`：rerank_score/ authority/summary/content | 打破 Bing 反爬脆弱性；模型可读长摘要；权威性可筛选 |
| `fetch_url`：`httpGet` + `decodeHtmlEntities`（HTML 实体解码，非 Markdown 转换） | Markdown 正文 + title | 无需自研 html→md；正文干净可直接进上下文 |
| 两者无鉴权、无计费 | 复用现有 API Key、按次计费 | 与 chat 同一凭据，无新密钥管理 |
| 无重试策略（一次 GET） | 官方明确规定可重试状态码 | 复用 `postWithRetry` / `retryConfig_` |

## 关键架构约束

### 1. 复用 DeepSeekClient，零新依赖

Search/Fetch 走 `config.baseUrl` + Bearer 鉴权 + JSON 请求，与 chat 完全同一套 curl 基础设施（`createEasyHandle` / `postWithRetry` / `BlockContext`，`deepseek.cpp` 150-241 行均为 private，类内新增方法直接可用）。**不引入新文件、新库、新证书逻辑。**

### 2. 工具名不变，executor 换实现

`registerSearchTool` / `registerFetchTool` 签名追加 `DeepSeekClient&` 参数；`app_tui.cpp` `setupTools()`（28-48 行）改为传 `this->client`（`app_tui.h` 133 行成员）。工具名 `web_search` / `fetch_url` 不变 → prompt 中的 `[TOOL_CALL]` 声明、`allowedTools` 过滤、历史会话均无需迁移。

### 3. 输出组织成模型易读文本

- `web_search` 结果：每组 `[score=x.x][authority=x] title\n url\n summary\n`（`content` 仅在 `need_content=true` 时输出，截断保护）
- `fetch_url` 结果：`title` + Markdown `content`，按 100KB 截断（沿用现有 `fetch_url` 截断上限）

### 4. 重试遵循官方指引

408 需加入可重试集合（现有 `isRetryableHttpStatus` 仅含 429/5xx，`deepseek.cpp` 339-343 行）；429/5xx 走既有 `retryConfig_` 指数退避；402/403/404 明确提示、不重试。

## 实施步骤

### Phase 1：DeepSeekClient 新增 Search/Fetch 方法

1. **`deepseek.h`** 新增公开方法：
   - `std::string searchWeb(const std::string& query, int count, bool needContent, std::string* errorOut)`
   - `std::string fetchWeb(const std::string& url, std::string* errorOut)`
   - 内部复用 `createEasyHandle` / `postWithRetry` / `BlockContext`（private，类内可见）
2. **`deepseek.cpp`** 实现：
   - endpoint：`baseUrl + "/v1/tools/search"`、`"/v1/tools/fetch"`
   - 请求体：构造 JSON `{query, count, need_content}` / `{url}`（`buildRequestBody` 是 chat 专用，不复用；新写小函数）
   - 响应解析：Search 遍历 `results[]` 组织文本；Fetch 取 `title`/`content`
   - 空结果：Search 200+空数组 → 返回 `"Error: No results found for ..."`（沿用语感）；Fetch 404 → 返回错误信息体
   - HTTP 非 200 时，将 `BlockContext.body`（OpenAI 兼容错误体）拼入返回，供 Agent 判断原因

### Phase 2：web_tools 替换实现

3. **`web_tools.h`**：签名改为 `void registerSearchTool(ToolRegistry&, DeepSeekClient&)`、`void registerFetchTool(ToolRegistry&, DeepSeekClient&)`
4. **`web_tools.cpp`**：
   - `registerSearchTool` executor：解析 `{query, count(默认10), need_content(默认false)}` → 调 `client.searchWeb` → 返回组织文本
   - `registerFetchTool` executor：解析 `{url}` → 校验 http/https 前缀 → 调 `client.fetchWeb` → 返回
   - 更新工具描述：`web_search` 写清"DeepSeek 官方搜索，按次计费，返回含权威性分数的结构化结果"；`fetch_url` 写清"以 Markdown 返回网页正文，按次计费"
   - 删除 `searchBing` / `httpGet` / `decodeHtmlEntities`（不再使用）
5. **`app_tui.cpp` `setupTools()`**（34-35 行）：`registerSearchTool(tools, client);` / `registerFetchTool(tools, client);`

### Phase 3：参数与裁剪

6. `web_search` 参数：`query`（必填，string）+ `count`（选填，number 1-50 默认 10）+ `need_content`（选填，boolean 默认 false）
7. 响应裁剪：`summary` 默认全文输出（模型友好）；`content` 截断到 50KB；Fetch `content` 截断到 100KB
8. 工具描述注明计费，抑制模型滥用（`exec_shell` 同类描述风格，见 `agent.cpp` 工具描述惯例）

### Phase 4：错误与重试

9. **`deepseek.cpp` `isRetryableHttpStatus`**：补充 408（当前仅 429 + 5xx）。注意：该函数被 chat 的 `postWithRetry` / `streamBlocking` 共用，补 408 后 chat 请求同样享受 408 重试（408 = 读取请求体超时，对 chat 同样合理），这是收益而非副作用。
10. Search/Fetch 方法内部：429/408/5xx 走 `postWithRetry` 自动重试（已有逻辑）；402/403/404 直接返回错误文本，不重试

### Phase 5：测试

11. 单测（`tests/unit`）：
    - Search 响应组织：mock JSON `results[]`（含/不含 content、空数组、rerank_score 各档）→ 断言输出文本
    - Fetch 响应组织：mock `{title, content}` → 断言输出
    - 错误透传：mock 402/403/404/429 响应体 → 断言错误文本
    - 参数校验：缺 query / count 越界 / url 非 http(s)
12. 手动联调：真实 API Key 搜索一次 + 抓取一次，确认鉴权与计费行为
13. `clean_and_build.bat` 全量构建回归

## 风险与对策

| 风险 | 对策 |
|------|------|
| Search/Fetch 按次计费，模型滥用拖高成本 | 工具描述注明收费 + 仅在真正需要实时/权威信息时调用；`need_content` 默认 false 减少正文开销 |
| Bing 抓取值机后果 | 快速替换后及时下线旧实现；替换窗口期 Bing 逻辑继续可用（新实现落地后删除） |
| 官方接口限速（Search 100 / Fetch 25 QPS） | 单用户单线程，远低于阈值；429 自动退避重试兜底 |
| Fetch 404（站点策略/无 Markdown 正文） | 返回错误信息含状态码，Agent 自行判断是否换源或放弃 |
| fetch 大页面 token 成本 | 100KB 截断 + 工具描述提示优先用 Search summary（600-800 字符） |
| 2026-11-01 计费生效前免费期行为差异 | 文档注明时间节点；上线即按计费设计 |


---

## 实施记录（v0.5.7）

状态：**已完成**。提交 `1b9db1f`。

### 与官方文档核对后的修正

| 本文档原文 | 官方实际 | 处理 |
|---|---|---|
| 结果字段 `publish_time` | `date`（RFC 3339，可能为 null） | 优先读 `date`，回退 `publish_time` |
| 未提 `site_name` / `site_icon` | 均存在，可为 null | 输出 `site:` 一行，null 时跳过 |
| `authority` 0~4 | 0~4，**-1 表示未知** | 原样输出，包括 -1 |
| 计费 5 元/千次 | 0.75 美元/千次成功请求 | 仅文档信息；工具描述只写"按次计费" |
| 未提 `lang:` 算子 | 支持 `lang:zh,en` | 请求 query 原样透传，无需代码 |

`label`/`ori_question`/`localization` 未暴露给模型：本文档 Phase 3 只要求 `query` / `count` / `need_content`。

### 代码落点

| 文件 | 内容 |
|---|---|
| `src/client/deepseek.h/.cpp` | `searchWeb` / `fetchWeb` / `postJson`（私有）/ 纯格式化 `formatSearchResults`、`formatFetchPage`、`formatApiError`；`isRetryableHttpStatus` 补 408 |
| `src/tools/web_tools.h/.cpp` | 工具名不变，仅换 executor；删除 `searchBing` / `httpGet` / `decodeHtmlEntities` |
| `src/tui/app_tui.cpp` | `registerSearchTool(tools, client)` / `registerFetchTool(tools, client)` |
| `tests/unit/test_web_api.cpp` | 7 组：结果渲染、need_content、空/异常 JSON、fetch 截断、错误映射 |

### 验证情况

- 单测：全部通过（无网络依赖，纯格式化层）。
- 输出裁剪：search 的 `content` 截 50KB，fetch 截 100KB，均带提示后缀。
- 空结果：Search 返回空数组 -> `Error: No results found for "<query>"`；Fetch 404 -> `Error: HTTP 404 (...): <message>`。
- **真机未验证**：仓库内的 `api_key.txt` 对 `api.deepseek.com` 返回 401（search/fetch/不存在路径同为 401，网关先鉴权），因此鉴权、计费与真实响应结构仍需你用一个有效 Key 跑一次。
