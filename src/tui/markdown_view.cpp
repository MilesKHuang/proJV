// proJV TUI -- Markdown FTXUI renderer implementation.
#include "markdown_view.h"
#include "markdown_text.h"

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
        case Style::Bold:   return Color::RGB(224, 184, 64);
        case Style::Italic: return Color::RGB(112, 184, 216);
        case Style::Code:   return Color::RGB(224, 120, 80);
        case Style::Link:   return Color::RGB(96, 144, 224);
        case Style::H1:     return Color::RGB(224, 184, 64);
        case Style::H2:     return Color::RGB(224, 168, 64);
        case Style::H3:     return Color::RGB(208, 192, 112);
        case Style::Quote:  return Color::RGB(136, 136, 160);
        case Style::Bullet: return Color::RGB(112, 168, 200);
        case Style::Ordered:return Color::RGB(112, 168, 200);
        case Style::HR:     return Color::RGB(48, 48, 72);
        case Style::CodeBlock:   return Color::RGB(224, 120, 80);
        case Style::TableHeader: return Color::RGB(224, 184, 64);
        case Style::TableCell:   return Color::RGB(212, 212, 224);
        case Style::Normal:
        default:            return Color::RGB(212, 212, 224);
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
