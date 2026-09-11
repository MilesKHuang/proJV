// proJV TUI -- Markdown FTXUI renderer implementation.
#include "markdown_view.h"
#include "markdown_text.h"
#include "theme_map.h"

#include <string>

namespace markdown_view {

using ftxui::Color;
using ftxui::Element;
using ftxui::Elements;
using markdown_text::Line;
using markdown_text::Segment;
using markdown_text::Style;

namespace {

Color styleColor(Style s) {
    switch (s) {
        case Style::Bold:   return theme_map::hexToColor("#E0B840");
        case Style::Italic: return theme_map::hexToColor("#70B8D8");
        case Style::Code:   return theme_map::hexToColor("#E07850");
        case Style::Link:   return theme_map::hexToColor("#6090E0");
        case Style::H1:     return theme_map::hexToColor("#E0B840");
        case Style::H2:     return theme_map::hexToColor("#E0A840");
        case Style::H3:     return theme_map::hexToColor("#D0C070");
        case Style::Quote:  return theme_map::hexToColor("#8888A0");
        case Style::Bullet: return theme_map::hexToColor("#70A8C8");
        case Style::Ordered:return theme_map::hexToColor("#70A8C8");
        case Style::HR:     return theme_map::hexToColor("#303048");
        case Style::CodeBlock:   return theme_map::hexToColor("#E07850");
        case Style::TableHeader: return theme_map::hexToColor("#E0B840");
        case Style::TableCell:   return theme_map::hexToColor("#D4D4E0");
        case Style::Normal:
        default:            return theme_map::hexToColor("#D4D4E0");
    }
}

Element renderLine(const Line& line) {
    if (line.segs.empty()) {
        return ftxui::text(" ");
    }

    const Segment& first = line.segs.front();
    if (first.style == Style::HR) {
        return ftxui::separator() | ftxui::color(styleColor(Style::HR));
    }

    if (first.style == Style::CodeBlock) {
        return ftxui::text(first.text) | ftxui::color(styleColor(Style::CodeBlock));
    }

    if (first.style == Style::H1 || first.style == Style::H2 || first.style == Style::H3) {
        return ftxui::text(first.text) | ftxui::bold | ftxui::color(styleColor(first.style));
    }

    if (first.style == Style::TableHeader) {
        Elements cells;
        for (size_t i = 0; i < line.segs.size(); ++i) {
            if (i > 0) cells.push_back(ftxui::text(" | "));
            cells.push_back(ftxui::text(line.segs[i].text));
        }
        return ftxui::hbox(std::move(cells)) | ftxui::color(styleColor(Style::TableHeader));
    }

    if (first.style == Style::TableCell) {
        Elements cells;
        for (size_t i = 0; i < line.segs.size(); ++i) {
            if (i > 0) cells.push_back(ftxui::text(" | "));
            cells.push_back(ftxui::text(line.segs[i].text));
        }
        return ftxui::hbox(std::move(cells));
    }

    if (first.style == Style::Bullet) {
        Elements els;
        els.push_back(ftxui::text("• ") | ftxui::color(styleColor(Style::Bullet)));
        std::string body;
        for (const auto& seg : line.segs) body += seg.text;
        els.push_back(ftxui::text(body));
        return ftxui::hbox(std::move(els));
    }

    if (first.style == Style::Ordered) {
        std::string body;
        for (const auto& seg : line.segs) body += seg.text;
        // first segment already carries the "N." prefix.
        return ftxui::text(body) | ftxui::color(styleColor(Style::Ordered));
    }

    // Paragraph / quote / inline: concatenate text, line-level color from the
    // first styled segment (inline multi-color degrades to one color).
    std::string body;
    Style lineStyle = Style::Normal;
    for (const auto& seg : line.segs) {
        body += seg.text;
        if (lineStyle == Style::Normal && seg.style != Style::Normal) {
            lineStyle = seg.style;
        }
    }
    return ftxui::text(body) | ftxui::color(styleColor(lineStyle));
}

} // namespace

Element renderMarkdown(const std::string& text) {
    auto lines = markdown_text::parseMarkdown(text);
    Elements els;
    for (const auto& line : lines) {
        els.push_back(renderLine(line));
    }
    return ftxui::vbox(std::move(els));
}

} // namespace markdown_view
