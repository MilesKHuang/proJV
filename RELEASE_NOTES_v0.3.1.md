## v0.3.1 — CJK font fix

### Fixes

- **Chinese text not showing**: font path was relative to CWD (working directory), causing CJK font load failure. Now resolved relative to executable directory, with fallback to system-installed Microsoft YaHei (`C:\Windows\Fonts\msyh.ttc`)
- **assets not copied to build**: added CMake post-build rule to automatically copy `assets/` folder to build output
- Context compaction now uses the main model instead of hardcoded `deepseek-chat`, ensuring context window compatibility
- Compaction `max_tokens` changed from fixed 2048 to user-configurable setting
- Fixed Release build error `C2065: 'curAgentState' undeclared identifier`

### Features

- Conversational AI coding assistant (DeepSeek API, SSE streaming)
- Tool execution: read/write files, shell, file search, web search, etc.
- Multi-step TODO management with automatic progress tracking
- Dual-strategy context compaction (LLM semantic + mechanical pruning)
- SQLite session persistence
- Dark theme with CJK font support
