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
        case Style::TableCell:   return theme_map::hexToColor(T.text);
        case Style::Normal:
        default:            return theme_map::hexToColor(T.text);
    }
}

Color borderColor() {
    return theme_map::hexToColor(ThemeManager::instance().current().border);
}

Color tableBorderColor() {
    return theme_map::hexToColor(ThemeManager::instance().current().headerActive);
}

Color codeBgColor() {
    return theme_map::hexToColor(ThemeManager::instance().current().mdCodeBg);
}

// --- CJK-aware soft-wrap -----------------------------------------------------
//
// FTXUI paragraph() splits on spaces only, so CJK text (which has no spaces)
// never wraps and gets clipped. We split into wrap tokens ourselves: ASCII
// words stay together, fullwidth (CJK) characters become their own token so
// they can break, and over-long words are split character-by-character so no
// single token can exceed the line width. flexbox() then re-flows the tokens
// on every resize (nothing is ever clipped).

int utf8CharLen(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

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

// Tokens wider than this are split character-by-character so they can wrap on
// even the narrowest terminals.
const size_t kMaxWordWidth = 20;

std::vector<std::string> splitForWrap(const std::string& text) {
    std::vector<std::string> tokens;
    std::string word;

    auto flushWord = [&]() {
        if (word.empty()) return;
        if (displayWidth(word) > kMaxWordWidth) {
            for (size_t i = 0; i < word.size();) {
                size_t l = utf8CharLen(static_cast<unsigned char>(word[i]));
                tokens.push_back(word.substr(i, l));
                i += l;
            }
        } else {
            tokens.push_back(std::move(word));
        }
        word.clear();
    };

    // Attach a space to the previous token (avoids leading spaces on wrapped
    // lines) and drops leading whitespace at the start of the paragraph.
    auto appendSpace = [&]() {
        if (!word.empty()) { word += ' '; }
        else if (!tokens.empty()) { tokens.back() += ' '; }
    };

    for (size_t i = 0; i < text.size();) {
        size_t len = utf8CharLen(static_cast<unsigned char>(text[i]));
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == ' ' || c == '\t') {
            appendSpace();
        } else if (len >= 3) {  // fullwidth (CJK / fullwidth punctuation)
            flushWord();
            tokens.push_back(text.substr(i, len));
        } else {
            word += text.substr(i, len);
        }
        i += len;
    }
    flushWord();
    return tokens;
}

Element wrapParagraph(const std::string& text) {
    auto tokens = splitForWrap(text);
    Elements items;
    items.reserve(tokens.size());
    for (auto& t : tokens) items.push_back(ftxui::text(std::move(t)));
    return ftxui::flexbox(std::move(items), ftxui::FlexboxConfig());
}

// Render a markdown table using ftxui::Table: box-drawing borders, bold
// header, per-column alignment (from the `:---:` / `---:` separator row).
// Cells soft-wrap (CJK-aware) so nothing clips and resize re-flows the table.
Element renderTable(const std::vector<std::vector<std::string>>& table,
                    const std::vector<int>& align) {
    if (table.empty() || table[0].empty()) {
        return ftxui::text(" ");
    }

    size_t cols = 0;
    for (const auto& row : table) cols = std::max(cols, row.size());

    // Build element cells so each cell wraps independently.
    std::vector<std::vector<Element>> grid;
    grid.reserve(table.size());
    for (size_t r = 0; r < table.size(); ++r) {
        std::vector<Element> row;
        row.reserve(cols);
        for (size_t c = 0; c < cols; ++c) {
            std::string cell = (c < table[r].size()) ? table[r][c] : "";
            Element e = wrapParagraph(cell);
            if (r == 0) {
                const auto& T = ThemeManager::instance().current();
                e = std::move(e) | ftxui::bold
                    | ftxui::color(theme_map::hexToColor(T.text))
                    | ftxui::bgcolor(theme_map::hexToColor(T.mdTableHdr));
            } else {
                e = std::move(e) | ftxui::color(styleColor(Style::TableCell));
            }
            row.push_back(std::move(e));
        }
        grid.push_back(std::move(row));
    }

    ftxui::Table ft(std::move(grid));
    // Outer box + inner column separators + header/body line. Border() alone
    // only draws the outer frame; the column dividers need SeparatorVertical.
    ft.SelectAll().Border(ftxui::DOUBLE, ftxui::color(tableBorderColor()));
    ft.SelectAll().SeparatorVertical(ftxui::DOUBLE, ftxui::color(tableBorderColor()));
    ft.SelectRow(0).SeparatorHorizontal(ftxui::DOUBLE, ftxui::color(tableBorderColor()));

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
        return ftxui::separatorHeavy() | ftxui::color(styleColor(Style::HR));
    }

    // Headings soft-wrap too (long headings re-flow on resize).
    if (first.style == Style::H1 || first.style == Style::H2 || first.style == Style::H3) {
        return wrapParagraph(first.text) | ftxui::bold | ftxui::color(styleColor(first.style));
    }

    // Bullet list: marker + soft-wrapped body.
    if (first.style == Style::Bullet) {
        std::string body;
        for (const auto& seg : line.segs) body += seg.text;
        return ftxui::hbox({
            ftxui::text("• ") | ftxui::color(styleColor(Style::Bullet)),
            wrapParagraph(body),
        });
    }

    // Ordered list: soft-wrapped body (leading "N." keeps its color).
    if (first.style == Style::Ordered) {
        std::string body;
        for (const auto& seg : line.segs) body += seg.text;
        return wrapParagraph(body) | ftxui::color(styleColor(Style::Ordered));
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
            wrapParagraph(body) | ftxui::color(styleColor(Style::Quote)),
        });
    }

    return wrapParagraph(body) | ftxui::color(styleColor(lineStyle));
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
        if (lines[i].tableHeader) {
            // Collect the whole table block (header + consecutive data rows).
            // Use cells[] so a cell containing inline styles stays ONE column
            // (the old flat "one seg = one cell" convention broke the column
            // count whenever a cell contained e.g. `code` or **bold**).
            auto collectRow = [](const Line& ln) {
                std::vector<std::string> row;
                if (!ln.cells.empty()) {
                    for (const auto& cell : ln.cells) {
                        std::string s;
                        for (const auto& sg : cell) s += sg.text;
                        row.push_back(std::move(s));
                    }
                } else {
                    // Fallback for hand-built Line objects without cells.
                    for (const auto& seg : ln.segs) row.push_back(seg.text);
                }
                return row;
            };

            std::vector<std::vector<std::string>> table;
            std::vector<int> align = lines[i].tableAlign;
            table.push_back(collectRow(lines[i]));

            size_t j = i + 1;
            while (j < lines.size() && !lines[j].cells.empty() && !lines[j].tableHeader) {
                table.push_back(collectRow(lines[j]));
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

namespace {
std::vector<std::string> splitLogicalLines(const std::string& text) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : text) {
        if (c == '\n') {
            lines.push_back(std::move(cur));
            cur.clear();
        } else if (c == '\r') {
            continue;
        } else {
            cur += c;
        }
    }
    lines.push_back(std::move(cur));
    return lines;
}
}  // namespace

ftxui::Element renderPlainText(const std::string& text) {
    ftxui::Elements els;
    for (auto& line : splitLogicalLines(text)) {
        els.push_back(line.empty() ? ftxui::text(" ") : wrapParagraph(line));
    }
    if (els.empty()) els.push_back(ftxui::text(" "));
    return ftxui::vbox(std::move(els));
}

ftxui::Element renderSoftWrappedInput(const std::string& text, int cursorByte, bool focused) {
    std::vector<std::string> lines = splitLogicalLines(text);

    int cursor = cursorByte < 0 ? 0
        : (cursorByte > static_cast<int>(text.size()) ? static_cast<int>(text.size()) : cursorByte);
    int cursorLine = 0;
    int col = cursor;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (col <= static_cast<int>(lines[i].size())) {
            cursorLine = static_cast<int>(i);
            break;
        }
        col -= static_cast<int>(lines[i].size()) + 1;
        cursorLine = static_cast<int>(i) + 1;
    }

    ftxui::Elements rows;
    rows.reserve(lines.size());
    for (size_t i = 0; i < lines.size(); ++i) {
        if (static_cast<int>(i) != cursorLine || !focused) {
            rows.push_back(lines[i].empty() ? ftxui::text(" ") : wrapParagraph(lines[i]));
            continue;
        }
        const std::string& line = lines[i];
        std::string before = line.substr(0, col);
        std::string at;
        if (col < static_cast<int>(line.size())) {
            int len = utf8CharLen(static_cast<unsigned char>(line[col]));
            at = line.substr(col, len);
        }
        std::string after = (col + at.size() <= line.size()) ? line.substr(col + at.size()) : "";

        auto beforeTokens = splitForWrap(before);
        auto afterTokens = splitForWrap(after);
        ftxui::Elements items;
        for (auto& t : beforeTokens) items.push_back(ftxui::text(std::move(t)));
        if (at.empty()) {
            items.push_back(ftxui::text(" ") | ftxui::focusCursorBarBlinking);
        } else {
            items.push_back(ftxui::text(at) | ftxui::focusCursorBarBlinking);
        }
        for (auto& t : afterTokens) items.push_back(ftxui::text(std::move(t)));
        rows.push_back(ftxui::flexbox(std::move(items), ftxui::FlexboxConfig()));
    }

    return ftxui::vbox(std::move(rows));
}

} // namespace markdown_view
