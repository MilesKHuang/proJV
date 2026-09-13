// proJV TUI -- Markdown FTXUI renderer (line-level styling; inline styles degrade
// to line-level color, text content is preserved 1:1).
#pragma once

#include <ftxui/dom/elements.hpp>

#include <string>

namespace markdown_view {

// Render markdown text into an FTXUI element.
ftxui::Element renderMarkdown(const std::string& text);

// Render plain text with CJK-aware soft-wrap (no markdown parsing). Splits on
// '\n'; each logical line wraps independently and re-flows on resize.
ftxui::Element renderPlainText(const std::string& text);

// Render input-box text with CJK-aware soft-wrap and an exact cursor. The
// glyph at byte offset `cursorByte` is drawn focused.
ftxui::Element renderSoftWrappedInput(const std::string& text, int cursorByte, bool focused);

} // namespace markdown_view
