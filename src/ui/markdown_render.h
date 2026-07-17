#pragma once
#include <string>
#include <functional>

// ---------------------------------------------------------------------------
// Simple line-by-line Markdown renderer for ImGui.
//
// Design goals:
//   1. Tables via ImGui::BeginTable
//   2. Code blocks via ImGui child with monospace
//   3. Bold/italic/code via color only (no font changes, preserves text)
//   4. Lean code, easy to debug and maintain
//
// Usage:
//   renderMarkdown(text, maxWidth, [](const std::string& url) {
//       ShellExecuteA(nullptr, "open", url.c_str(), ...);
//   });
// ---------------------------------------------------------------------------

using MarkdownLinkCallback = std::function<void(const std::string& url)>;

// Render markdown text at the current ImGui cursor position.
// maxWidth: available content width for wrapping and table sizing.
// onLink: optional callback for clicked links (default: no-op).
void renderMarkdown(const std::string& text, float maxWidth,
                    MarkdownLinkCallback onLink = nullptr);
