## v0.3.0 — Initial Release

A Windows desktop AI coding assistant built with ImGui + DirectX11, powered by the DeepSeek Chat API.

### Features

- **Conversational AI coding assistant**: streaming chat interface backed by DeepSeek models
- **Tool execution**: read/write files, shell commands, file search, web search, and more
- **TODO management**: multi-step task breakdown with automatic progress tracking
- **Context management**: dual-strategy compaction (LLM semantic compression + mechanical pruning) to prevent context overflow in long sessions
- **Session persistence**: SQLite-backed chat history storage
- **Dark theme + CJK font**: Chinese/Japanese/Korean text support out of the box

### Fixes

- Context compaction now uses the main model instead of hardcoded `deepseek-chat`, ensuring context window compatibility
- Compaction `max_tokens` changed from fixed 2048 to user-configurable setting
- Fixed Release build error `C2065: 'curAgentState' undeclared identifier`

### Tech Stack

- C++20 / CMake / MSVC
- Dear ImGui + DirectX11 + Win32
- DeepSeek Chat API (SSE streaming)
- SQLiteCpp / libcurl

### Usage

1. Set your DeepSeek API Key in `config.toml`
2. Build and run (see `README.md`)
3. Type your request — the AI breaks it down and executes step by step
