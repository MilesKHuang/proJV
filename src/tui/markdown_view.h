// proJV TUI -- Markdown FTXUI renderer (line-level styling; inline styles degrade
// to line-level color, text content is preserved 1:1).
#pragma once

#include <ftxui/dom/elements.hpp>

#include <string>

namespace markdown_view {

// Render markdown text into an FTXUI element.
ftxui::Element renderMarkdown(const std::string& text);

} // namespace markdown_view
