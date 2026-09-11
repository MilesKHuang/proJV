// proJV TUI -- Markdown FTXUI renderer implementation.
#include "markdown_view.h"
#include "markdown_text.h"
#include "theme_map.h"
#include "theme_manager.h"

#include <string>
#include <vector>

namespace markdown_view {

using ftxui::Color;
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

// Display width of a UTF-8 string (ASCII=1, CJK/fullwidth=2), for table alignment.
size_t displayWidth(const std::string& s) {
    size_t w = 0;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { w += 1; i += 1; }
        else if ((c & 0xE0) == 0xC0) { w += 1; i += 2; }
        else if ((c & 0xF0) == 0xE0) { w += 2; i += 3; }
        else if ((c & 0xF8) == 0xF0) { w += 2; i += 4; }
        else { w += 1; i += 1; }
    }
    return w;
}

std::string padTo(const std::string& s, size_t width) {
    size_t w = displayWidth(s);
    if (w >= width) return s;
    return s + std::string(width - w, ' ');
}

// Render an aligned table (header + separator + rows).
Element renderTable(const std::vector<std::vector<std::string>>& table) {
    size_t cols = 0;
    for (const auto& row : table) cols = std::max(cols, row.size());

    std::vector<size_t> widths(cols, 0);
    for (const auto& row : table) {
        for (size_t c = 0; c < row.size(); ++c) {
            widths[c] = std::max(widths[c], displayWidth(row[c]));
        }
    }

    Elements rows;
    for (size_t r = 0; r < table.size(); ++r) {
        std::string line = "|";
        for (size_t c = 0; c < cols; ++c) {
            std::string cell = (c < table[r].size()) ? table[r][c] : "";
            line += " " + padTo(cell, widths[c]) + " |";
        }
        rows.push_back(ftxui::text(line) |
            ftxui::color(r == 0 ? styleColor(Style::TableHeader) : styleColor(Style::TableCell)));

        if (r == 0) {
            std::string sep = "|";
            for (size_t c = 0; c < cols; ++c) {
                sep += std::string(widths[c] + 2, '-') + "|";
            }
            rows.push_back(ftxui::text(sep) | ftxui::color(styleColor(Style::HR)));
        }
    }
    return ftxui::vbox(std::move(rows));
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
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].segs.empty()) {
            els.push_back(ftxui::text(" "));
            continue;
        }
        if (lines[i].segs.front().style == Style::TableHeader) {
            // Collect the whole table block (header + consecutive data rows).
            std::vector<std::vector<std::string>> table;
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
            els.push_back(renderTable(table));
            i = j - 1;
        } else {
            els.push_back(renderLine(lines[i]));
        }
    }
    return ftxui::vbox(std::move(els));
}

} // namespace markdown_view
