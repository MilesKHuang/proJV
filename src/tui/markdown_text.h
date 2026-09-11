// proJV TUI -- Markdown text model: parse markdown into styled lines.
//
// 1:1 extraction of the block/inline rules in src/ui/markdown_render.cpp,
// expressed as a UI-agnostic intermediate form so the FTXUI renderer can color
// segments without reimplementing the grammar.
#pragma once

#include <string>
#include <vector>

namespace markdown_text {

enum class Style {
    Normal, Bold, Italic, Code, Link,
    H1, H2, H3, Quote, Bullet, Ordered,
    HR, CodeBlock, TableHeader, TableCell
};

struct Segment {
    std::string text;
    Style style = Style::Normal;
    std::string url;   // non-empty only for Link segments
};

struct Line {
    std::vector<Segment> segs;
};

// Parse markdown text into structured lines. Pure function (no UI deps).
std::vector<Line> parseMarkdown(const std::string& text);

} // namespace markdown_text
