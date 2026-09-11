// proJV TUI -- Markdown FTXUI renderer implementation.
//
// Renders the UI-agnostic markdown_text model into FTXUI elements:
//   * Tables use ftxui::Table (real box-drawing borders + column alignment).
//   * Paragraphs / list items / quotes use paragraph() so long lines soft-wrap
//     and re-wrap automatically when the terminal is resized.
//   * Inline styles degrade to line-level color (text content preserved 1:1).
#include "markdown_view.h"
#include "markdown_text.h"
#include "theme_map.h"
#include "theme_manager.h"

#include <ftxui/dom/table.hpp>

#include <string>
#include <vector>

namespace markdown_view {

using ftxui::Color;
using ftxui::Decorator;
using ftxui::Element;
using ftxui::Elements;
using markdown_text::Line;
using markdown_text::Segment;
using markdown_text::Style;

namespace {

Color styleColor(Style s) {
    const auto& T = ThemeManager::instance().current();
    switch (s) {
        case Style::Bold:   return theme_map::hexToColor(T.mdBold);
        case Style::Italic: return theme_map::hexToColor(T.mdItalic);
        case Style::Code:   return theme_map::hexToColor(T.mdCode);
        case Style::Link:   return theme_map::hexToColor(T.mdLink);
        case Style::H1:     return theme_map::hexToColor(T.mdH1);
        case Style::H2:     return theme_map::hexToColor(T.mdH2);
        case Style::H3:     return theme_map::hexToColor(T.mdH3);
        case Style::Quote:  return theme_map::hexToColor(T.mdQuote);
        case Style::Bullet: return theme_map::hexToColor(T.mdBullet);
        case Style::Ordered:return theme_map::hexToColor(T.mdBullet);
        case Style::HR:     return theme_map::hexToColor(T.mdHR);
        case Style::CodeBlock:   return theme_map::hexToColor(T.mdCode);
        case Style::TableHeader: return theme_map::hexToColor(T.mdTableHdr);
        case Style::TableCell:   return theme_map::hexToColor(T.text);
        case Style::Normal:
        default:            return theme_map::hexToColor(T.text);
    }
}

Color borderColor() {
    return theme_map::hexToColor(ThemeManager::instance().current().border);
}

Color codeBgColor() {
    return theme_map::hexToColor(ThemeManager::instance().current().mdCodeBg);
}

// Render a markdown table using ftxui::Table: box-drawing borders, bold
// header, per-column alignment (from the `:---:` / `---:` separator row).
Element renderTable(const std::vector<std::vector<std::string>>& table,
                    const std::vector<int>& align) {
    if (table.empty() || table[0].empty()) {
        return ftxui::text(" ");
    }

    size_t cols = 0;
    for (const auto& row : table) cols = std::max(cols, row.size());

    ftxui::Table ft(table);
    ft.SelectAll().Border(ftxui::LIGHT, ftxui::color(borderColor()));
    ft.SelectRow(0).DecorateCells([&](Element e) {
        return std::move(e) | ftxui::bold | ftxui::color(styleColor(Style::TableHeader));
    });
    ft.SelectRow(0).SeparatorHorizontal(ftxui::LIGHT);

    // Per-column alignment (mirrors legacy markdown table alignment rules).
    for (size_t c = 0; c < align.size() && c < cols; ++c) {
        Decorator d = ftxui::nothing;
        if (align[c] == 1) d = ftxui::hcenter;
        else if (align[c] == 2) d = ftxui::align_right;
        ft.SelectColumn(static_cast<int>(c)).DecorateCells(d);
    }

    return ft.Render();
}

Element renderLine(const Line& line) {
    if (line.segs.empty()) {
        return ftxui::text(" ");
    }

    const Segment& first = line.segs.front();
    if (first.style == Style::HR) {
        return ftxui::separator() | ftxui::color(styleColor(Style::HR));
    }

    // Headings are short; keep as-is (bold + theme color).
    if (first.style == Style::H1 || first.style == Style::H2 || first.style == Style::H3) {
        return ftxui::text(first.text) | ftxui::bold | ftxui::color(styleColor(first.style));
    }

    // Bullet list: marker + soft-wrapped body.
    if (first.style == Style::Bullet) {
        std::string body;
        for (const auto& seg : line.segs) body += seg.text;
        return ftxui::hbox({
            ftxui::text("• ") | ftxui::color(styleColor(Style::Bullet)),
            ftxui::paragraph(body),
        });
    }

    // Ordered list: soft-wrapped body (leading "N." keeps its color).
    if (first.style == Style::Ordered) {
        std::string body;
        for (const auto& seg : line.segs) body += seg.text;
        return ftxui::paragraph(body) | ftxui::color(styleColor(Style::Ordered));
    }

    // Paragraph / quote / inline: soft-wrap, line-level color from the first
    // styled segment (inline multi-color degrades to one color).
    std::string body;
    Style lineStyle = Style::Normal;
    for (const auto& seg : line.segs) {
        body += seg.text;
        if (lineStyle == Style::Normal && seg.style != Style::Normal) {
            lineStyle = seg.style;
        }
    }

    // Blockquote keeps its legacy left bar (mdQuoteBar), colored like mdQuote.
    if (lineStyle == Style::Quote) {
        const auto& T = ThemeManager::instance().current();
        return ftxui::hbox({
            ftxui::text("│ ") | ftxui::color(theme_map::hexToColor(T.mdQuoteBar)),
            ftxui::paragraph(body) | ftxui::color(styleColor(Style::Quote)),
        });
    }

    return ftxui::paragraph(body) | ftxui::color(styleColor(lineStyle));
}

} // namespace

Element renderMarkdown(const std::string& text) {
    auto lines = markdown_text::parseMarkdown(text);
    Elements els;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].segs.empty()) {
            els.push_back(ftxui::text(" "));
            continue;
        }
        if (lines[i].segs.front().style == Style::TableHeader) {
            // Collect the whole table block (header + consecutive data rows).
            std::vector<std::vector<std::string>> table;
            std::vector<int> align = lines[i].tableAlign;

            std::vector<std::string> header;
            for (const auto& seg : lines[i].segs) header.push_back(seg.text);
            table.push_back(std::move(header));

            size_t j = i + 1;
            while (j < lines.size() && !lines[j].segs.empty() &&
                   lines[j].segs.front().style == Style::TableCell) {
                std::vector<std::string> row;
                for (const auto& seg : lines[j].segs) row.push_back(seg.text);
                table.push_back(std::move(row));
                ++j;
            }
            els.push_back(renderTable(table, align));
            i = j - 1;
        } else if (lines[i].segs.front().style == Style::CodeBlock) {
            // Collect consecutive code lines into one background block
            // (mirrors legacy renderCodeBlock's mdCodeBg rect + border).
            Elements codeLines;
            size_t j = i;
            while (j < lines.size() && !lines[j].segs.empty() &&
                   lines[j].segs.front().style == Style::CodeBlock) {
                codeLines.push_back(ftxui::text(lines[j].segs.front().text) |
                    ftxui::color(styleColor(Style::CodeBlock)));
                ++j;
            }
            Element block = ftxui::vbox(std::move(codeLines)) |
                ftxui::bgcolor(codeBgColor()) |
                ftxui::borderStyled(borderColor());
            els.push_back(std::move(block));
            i = j - 1;
        } else {
            els.push_back(renderLine(lines[i]));
        }
    }
    return ftxui::vbox(std::move(els));
}

} // namespace markdown_view
