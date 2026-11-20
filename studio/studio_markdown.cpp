// proJV studio -- markdown drawing (see studio_markdown.h).
#include "studio_markdown.h"

#include <algorithm>
#include <cstring>

#include "studio_theme.h"
#include "tui/theme_manager.h"

namespace studio_md {
namespace {

using markdown_text::Segment;
using markdown_text::Style;

// Length in bytes of the UTF-8 code point starting at `i`.
size_t cpLen(const std::string& s, size_t i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c >= 0xF0) return 4;
    if (c >= 0xE0) return 3;
    if (c >= 0xC0) return 2;
    return 1;
}

bool isSpace(char c) {
    return c == ' ' || c == '\t';
}

ImU32 styleColor(Style st) {
    const ThemeColors& T = ThemeManager::instance().current();
    switch (st) {
    case Style::H1:
        return studio_theme::fromHex(T.mdH1);
    case Style::H2:
        return studio_theme::fromHex(T.mdH2);
    case Style::H3:
        return studio_theme::fromHex(T.mdH3);
    case Style::Bold:
        return studio_theme::fromHex(T.mdBold);
    case Style::Italic:
        return studio_theme::fromHex(T.mdItalic);
    case Style::Code:
    case Style::CodeBlock:
        return studio_theme::fromHex(T.mdCode);
    case Style::Link:
        return studio_theme::fromHex(T.mdLink);
    case Style::Quote:
        return studio_theme::fromHex(T.mdQuote);
    case Style::Bullet:
    case Style::Ordered:
        return studio_theme::fromHex(T.mdBullet);
    case Style::HR:
        return studio_theme::fromHex(T.mdHR);
    case Style::TableHeader:
        return studio_theme::fromHex(T.mdTableHdr);
    case Style::TableCell:
    case Style::Normal:
    default:
        return studio_theme::fromHex(T.text);
    }
}

bool isHeading(Style s) {
    return s == Style::H1 || s == Style::H2 || s == Style::H3;
}

void drawRun(const std::string& text, Style style, ImU32 overrideColor) {
    const ImU32 col = overrideColor ? overrideColor : styleColor(style);
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextUnformatted(text.c_str());
    if (style == Style::Bold || isHeading(style)) {
        // ImGui has no bold face here (one monospace + one CJK face): a 1px
        // offset redraw reads as weight at these sizes and costs nothing.
        const ImVec2 p = ImGui::GetItemRectMin();
        ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + 1.0f, p.y),
                                            ImGui::GetColorU32(ImGuiCol_Text), text.c_str());
    }
    ImGui::PopStyleColor();
}

} // namespace

float textWidth(const std::string& utf8) {
    if (utf8.empty()) return 0.0f;
    return ImGui::CalcTextSize(utf8.c_str(), nullptr, false, -1.0f).x;
}

std::vector<std::vector<Segment>> wrap(const std::vector<Segment>& segs, float wrapWidth) {
    std::vector<std::vector<Segment>> lines;
    std::vector<Segment> cur;
    float curW = 0.0f;
    if (wrapWidth < 8.0f) wrapWidth = 8.0f;

    auto emit = [&] {
        lines.push_back(cur);
        cur.clear();
        curW = 0.0f;
    };

    for (const auto& seg : segs) {
        // Tokenize: a run of non-space bytes is one token; the space that follows
        // stays attached, so a line break drops the space together with the word.
        size_t i = 0;
        while (i < seg.text.size()) {
            const size_t start = i;
            while (i < seg.text.size() && !isSpace(seg.text[i])) i += cpLen(seg.text, i);
            while (i < seg.text.size() && isSpace(seg.text[i])) ++i;
            const std::string token = seg.text.substr(start, i - start);
            const float w = textWidth(token);

            if (w > wrapWidth) {
                // A single token wider than the line (a path, a URL, a CJK run)
                // is broken by code point.
                std::string piece;
                size_t k = 0;
                while (k < token.size()) {
                    const size_t len = cpLen(token, k);
                    const std::string next = token.substr(k, len);
                    if (!piece.empty() && textWidth(piece + next) > wrapWidth) {
                        Segment s;
                        s.text = piece;
                        s.style = seg.style;
                        s.url = seg.url;
                        cur.push_back(s);
                        emit();
                        piece.clear();
                    }
                    piece += next;
                    k += len;
                }
                if (!piece.empty()) {
                    Segment s;
                    s.text = piece;
                    s.style = seg.style;
                    s.url = seg.url;
                    cur.push_back(s);
                    curW = textWidth(piece);
                }
                continue;
            }

            if (curW + w > wrapWidth && !cur.empty()) emit();
            Segment s;
            s.text = token;
            s.style = seg.style;
            s.url = seg.url;
            cur.push_back(s);
            curW += w;
        }
    }
    if (!cur.empty()) lines.push_back(cur);
    if (lines.empty()) lines.push_back({});
    return lines;
}

void drawMarkdown(const std::string& md, float wrapWidth) {
    const auto parsed = markdown_text::parseMarkdown(md);
    for (const auto& line : parsed) {
        if (line.tableHeader || !line.cells.empty()) {
            // Tables: an even column split across the available width. The TUI
            // shows the same cells; only the column metrics differ.
            const int cols = static_cast<int>(line.cells.size());
            if (cols <= 0) continue;
            const float colW = wrapWidth / static_cast<float>(cols);
            const ImVec2 rowPos = ImGui::GetCursorScreenPos();
            for (int c = 0; c < cols; ++c) {
                std::string cell;
                for (const auto& s : line.cells[static_cast<size_t>(c)]) cell += s.text;
                const ImVec2 cellPos(rowPos.x + colW * static_cast<float>(c), rowPos.y);
                ImGui::PushClipRect(cellPos, ImVec2(cellPos.x + colW - 4.0f,
                                                    cellPos.y + ImGui::GetTextLineHeight() + 2.0f),
                                    true);
                ImGui::SetCursorScreenPos(cellPos);
                drawRun(cell, line.tableHeader ? Style::TableHeader : Style::TableCell, 0);
                ImGui::PopClipRect();
            }
            ImGui::SetCursorScreenPos(ImVec2(rowPos.x, rowPos.y + ImGui::GetTextLineHeight()));
            continue;
        }
        if (line.segs.empty()) {
            ImGui::Dummy(ImVec2(1.0f, ImGui::GetTextLineHeight() * 0.5f));
            continue;
        }
        // A rule is a drawn line, not a row of dashes.
        if (line.segs.size() == 1 && line.segs[0].style == Style::HR) {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddLine(p, ImVec2(p.x + wrapWidth, p.y + 1.0f),
                                                studio_theme::field(&ThemeColors::mdHR));
            ImGui::Dummy(ImVec2(1.0f, 3.0f));
            continue;
        }

        // A code block keeps its background across the whole line.
        const bool codeBlock =
            std::all_of(line.segs.begin(), line.segs.end(), [](const Segment& s) {
                return s.style == Style::CodeBlock;
            });
        const bool quote = line.segs[0].style == Style::Quote;

        const auto rows = wrap(line.segs, wrapWidth - (quote ? 7.0f : 0.0f));
        for (const auto& row : rows) {
            const ImVec2 rowStart = ImGui::GetCursorScreenPos();
            const float rowH = ImGui::GetTextLineHeight();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (codeBlock)
                dl->AddRectFilled(rowStart, ImVec2(rowStart.x + wrapWidth, rowStart.y + rowH + 2.0f),
                                  studio_theme::field(&ThemeColors::mdCodeBg));
            if (quote) {
                dl->AddRectFilled(rowStart, ImVec2(rowStart.x + 3.0f, rowStart.y + rowH),
                                  studio_theme::field(&ThemeColors::mdQuoteBar));
                ImGui::SetCursorScreenPos(ImVec2(rowStart.x + 7.0f, rowStart.y));
            }
            if (row.empty()) {
                ImGui::NewLine();
            } else {
                for (size_t s = 0; s < row.size(); ++s) {
                    if (s > 0) ImGui::SameLine(0.0f, 0.0f);
                    drawRun(row[s].text, row[s].style, 0);
                }
            }
        }
    }
}

void drawPlain(const std::string& text, float wrapWidth, ImU32 color) {
    std::vector<Segment> segs;
    Segment whole;
    whole.text = text;
    whole.style = Style::Normal;
    segs.push_back(whole);
    for (const auto& row : wrap(segs, wrapWidth)) {
        if (row.empty()) {
            ImGui::Dummy(ImVec2(1.0f, ImGui::GetTextLineHeight() * 0.5f));
            continue;
        }
        for (size_t s = 0; s < row.size(); ++s) {
            if (s > 0) ImGui::SameLine(0.0f, 0.0f);
            drawRun(row[s].text, Style::Normal, color);
        }
    }
}

void drawHint(const std::string& text, float wrapWidth) {
    (void)wrapWidth;
    // Secondary text goes through the contrast guard: a hand-authored theme's
    // "disabled" grey is often below 4.5:1, which is what made the Light theme
    // look washed out.
    ImGui::PushStyleColor(ImGuiCol_Text,
                          studio_theme::readable(studio_theme::field(&ThemeColors::textDisabled),
                                                 studio_theme::field(&ThemeColors::windowBg)));
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopStyleColor();
}

} // namespace studio_md
