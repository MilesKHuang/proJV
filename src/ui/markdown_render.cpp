#include "markdown_render.h"
#include "ui/theme.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <vector>
#include <cctype>
#include <cstring>

// ============================================================================
// Internal content padding (inside green bubble)
// ============================================================================
static const float PAD_X = 6.0f;

// ============================================================================
// Utility
// ============================================================================
static float fontSz()  { return ImGui::GetFontSize(); }
static float lineH()   { return fontSz() * 1.35f; }

static const char* skipSpace(const char* s) {
    while (*s == ' ' || *s == '\t') ++s;
    return s;
}
static bool startsWith(const char* s, const char* prefix) {
    while (*prefix) { if (*s != *prefix) return false; ++s; ++prefix; }
    return true;
}

// ============================================================================
// Inline segment struct
// ============================================================================
struct InlineSeg {
    std::string text;
    ImVec4      color;
    bool        isLink;
    std::string url;
};

// Parse one line into InlineSeg vector
static void parseInlineSegs(const char* line, const char* lineEnd,
                            std::vector<InlineSeg>& out)
{
    const char* p = line;
    const char* segStart = line;
    auto pushNormal = [&](const char* end) {
        if (end > segStart)
            out.push_back({std::string(segStart, end),
                           ImGui::GetStyleColorVec4(ImGuiCol_Text),
                           false, ""});
        segStart = end;
    };
    while (p < lineEnd) {
        if (*p == '`') {
            const char* e = p + 1;
            while (e < lineEnd && *e != '`') ++e;
            if (e < lineEnd && e > p + 1) {
                pushNormal(p);
                out.push_back({std::string(p + 1, e), ThemeColors::toVec4(ThemeManager::instance().current().mdCode), false, ""});
                segStart = p = e + 1; continue;
            }
        }
        if (p + 1 < lineEnd && *p == '*' && *(p + 1) == '*') {
            const char* e = p + 2;
            while (e + 1 < lineEnd && !(*e == '*' && *(e + 1) == '*')) ++e;
            if (e + 1 < lineEnd && e > p + 2) {
                pushNormal(p);
                out.push_back({std::string(p + 2, e), ThemeColors::toVec4(ThemeManager::instance().current().mdBold), false, ""});
                segStart = p = e + 2; continue;
            }
        }
        if (*p == '*' && (p == line || *(p - 1) != '*') &&
            (p + 1 >= lineEnd || *(p + 1) != '*')) {
            const char* e = p + 1;
            while (e < lineEnd && *e != '*') ++e;
            if (e < lineEnd && e > p + 1) {
                pushNormal(p);
                out.push_back({std::string(p + 1, e), ThemeColors::toVec4(ThemeManager::instance().current().mdItalic), false, ""});
                segStart = p = e + 1; continue;
            }
        }
        if (*p == '[') {
            const char* rb = p + 1;
            while (rb < lineEnd && *rb != ']') ++rb;
            if (rb < lineEnd && rb > p + 1 &&
                rb + 1 < lineEnd && *(rb + 1) == '(') {
                const char* rp = rb + 2;
                while (rp < lineEnd && *rp != ')') ++rp;
                if (rp < lineEnd) {
                    pushNormal(p);
                    out.push_back({std::string(p + 1, rb), ThemeColors::toVec4(ThemeManager::instance().current().mdLink),
                                   true, std::string(rb + 2, rp)});
                    segStart = p = rp + 1; continue;
                }
            }
        }
        ++p;
    }
    pushNormal(lineEnd);
}

// ============================================================================
// Render inline segments with ImDrawList wrapping.
// Advances only cursor Y, preserves caller's cursor X.
// Returns final Y position after the line.
// ============================================================================
static float renderInlineWrapped(const std::vector<InlineSeg>& segs,
                                 float startX, float wrapEndX,
                                 MarkdownLinkCallback& onLink)
{
    if (segs.empty()) return ImGui::GetCursorScreenPos().y;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    float x = startX;
    float y = ImGui::GetCursorScreenPos().y;

    for (size_t i = 0; i < segs.size(); ++i) {
        const auto& seg = segs[i];
        float tw = ImGui::CalcTextSize(seg.text.c_str()).x;
        if (x + tw > wrapEndX && x > startX + 1.0f) {
            y += lineH();
            x = startX;
        }
        ImU32 col = ImGui::ColorConvertFloat4ToU32(seg.color);
        dl->AddText(ImGui::GetFont(), fontSz(), ImVec2(x, y), col,
                    seg.text.c_str());
        if (seg.isLink) {
            dl->AddLine(ImVec2(x, y + fontSz() + 1),
                        ImVec2(x + tw, y + fontSz() + 1),
                        ImGui::ColorConvertFloat4ToU32(ThemeColors::toVec4(ThemeManager::instance().current().mdLinkUnder)));
            ImGui::SetCursorScreenPos(
                ImVec2(x, y - ImGui::GetStyle().ItemSpacing.y));
            ImGui::InvisibleButton("##mdlink", ImVec2(tw, fontSz()));
            if (ImGui::IsItemClicked() && onLink) onLink(seg.url);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", seg.url.c_str());
        }
        x += tw;
    }
    y += lineH();
    // Register cursor advancement as a widget so parent AutoResizeY tracks it
    ImGui::SetCursorScreenPos(ImVec2(startX, y));
    ImGui::Dummy(ImVec2(1.0f, 0.0f));
    return y;
}

// ============================================================================
// Inline renderer for table cells (no wrap, just color)
// ============================================================================
static void renderInlineCell(const std::vector<InlineSeg>& segs)
{
    for (size_t i = 0; i < segs.size(); ++i) {
        const auto& seg = segs[i];
        ImGui::PushStyleColor(ImGuiCol_Text, seg.color);
        ImGui::TextUnformatted(seg.text.c_str());
        ImGui::PopStyleColor();
        if (i + 1 < segs.size()) ImGui::SameLine(0, 0);
    }
}

// ============================================================================
// Render code block — draws background rect + lines via ImDrawList.
// NO BeginChild: avoids nested-child AutoResizeY issues.
// Minimum height wrap, no scroll.
// ============================================================================
static void renderCodeBlock(const std::vector<std::string>& lines,
                            float maxWidth)
{
    if (lines.empty()) return;

    int n = (int)lines.size();
    float blockW = maxWidth - 8.0f;
    float blockH = n * lineH() + fontSz() * 0.6f;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 cp = ImGui::GetCursorScreenPos();

    // Background rect
    dl->AddRectFilled(cp, ImVec2(cp.x + blockW, cp.y + blockH),
                      ImGui::ColorConvertFloat4ToU32(ThemeColors::toVec4(ThemeManager::instance().current().mdCodeBg)), 4.0f);
    // Subtle border
    dl->AddRect(cp, ImVec2(cp.x + blockW, cp.y + blockH),
                IM_COL32(60, 65, 75, 255), 4.0f);

    // Draw each line
    float y = cp.y + fontSz() * 0.25f;
    for (const auto& ln : lines) {
        dl->AddText(ImGui::GetFont(), fontSz(),
                    ImVec2(cp.x + 6.0f, y),
                    ImGui::ColorConvertFloat4ToU32(ThemeColors::toVec4(ThemeManager::instance().current().mdCode)),
                    ln.c_str());
        y += lineH();
    }

    // Advance cursor past the block
    ImGui::SetCursorScreenPos(ImVec2(cp.x, cp.y + blockH + fontSz() * 0.3f));
    ImGui::Dummy(ImVec2(1.0f, 0.0f));
}

// ============================================================================
// Table accumulator and renderer
// ============================================================================
struct TableData {
    std::vector<std::string>            headers;
    std::vector<std::vector<InlineSeg>> headerSegs;
    std::vector<int>                    align;
    std::vector<std::vector<std::vector<InlineSeg>>> rows;
    std::vector<float>                  colWidths;
};

static void renderTable(TableData& tbl, float maxWidth)
{
    if (tbl.headers.empty()) return;

    int nCols = (int)tbl.headers.size();
    tbl.colWidths.assign(nCols, 0.0f);
    float pad = ImGui::GetStyle().CellPadding.x * 2 + 8.0f;

    auto measureSegs = [pad](const std::vector<InlineSeg>& segs) -> float {
        float w = 0;
        for (auto& s : segs) w += ImGui::CalcTextSize(s.text.c_str()).x;
        return w + pad;
    };

    for (int c = 0; c < nCols; ++c)
        tbl.colWidths[c] = measureSegs(tbl.headerSegs[c]);

    for (auto& row : tbl.rows) {
        for (int c = 0; c < nCols && c < (int)row.size(); ++c) {
            float w = measureSegs(row[c]);
            if (w > tbl.colWidths[c]) tbl.colWidths[c] = w;
        }
    }

    float avail = maxWidth - 16.0f;
    for (int c = 0; c < nCols; ++c) {
        if (tbl.colWidths[c] < 60.0f) tbl.colWidths[c] = 60.0f;
    }
    float total = 0;
    for (int c = 0; c < nCols; ++c) total += tbl.colWidths[c];
    if (total > avail && total > 0) {
        float scale = avail / total;
        for (int c = 0; c < nCols; ++c)
            tbl.colWidths[c] = (tbl.colWidths[c] * scale < 40.0f)
                               ? 40.0f : tbl.colWidths[c] * scale;
    }

    ImU32 flags = ImGuiTableFlags_Borders |
                  ImGuiTableFlags_RowBg |
                  ImGuiTableFlags_ScrollY |
                  ImGuiTableFlags_SizingFixedFit;

    // Height: at most 10 visible data rows
    int visRows = (int)tbl.rows.size();
    float h = ((visRows > 0 ? (std::min)(visRows, 10) : 1) + 1) * lineH() + fontSz();

    ImGui::PushID(&tbl);
    if (ImGui::BeginTable("##tbl", nCols, flags, ImVec2(maxWidth - 8.0f, h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        for (int c = 0; c < nCols; ++c) {
            int a = (c < (int)tbl.align.size()) ? tbl.align[c] : 0;
            ImGui::TableSetupColumn(tbl.headers[c].c_str(), a, tbl.colWidths[c]);
        }
        ImGui::TableHeadersRow();

        for (auto& row : tbl.rows) {
            // Skip rows where all cells produce empty text
            bool anyText = false;
            for (auto& cellSegs : row) {
                for (auto& s : cellSegs)
                    if (!s.text.empty()) { anyText = true; break; }
                if (anyText) break;
            }
            if (!anyText) continue;

            ImGui::TableNextRow();
            for (int col = 0; col < nCols; ++col) {
                ImGui::TableSetColumnIndex(col);
                if (col < (int)row.size() && !row[col].empty())
                    renderInlineCell(row[col]);
            }
        }
        ImGui::EndTable();
    }
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0, fontSz() * 0.3f));
}

// Parse alignment from separator row
static std::vector<int> parseTableAlign(const char* line, const char* lineEnd,
                                         int expectedCols)
{
    std::vector<int> result;
    const char* p = line;
    while (p < lineEnd) {
        if (*p == '|') {
            ++p;
            const char* segStart = p;
            while (p < lineEnd && *p != '|') ++p;
            while (segStart < p && *segStart == ' ') ++segStart;
            const char* segEnd = p;
            while (segEnd > segStart && *(segEnd - 1) == ' ') --segEnd;
            bool lc = (segStart < segEnd && *segStart == ':');
            bool rc = (segStart < segEnd && *(segEnd - 1) == ':');
            if (lc && rc)       result.push_back(1);
            else if (rc)        result.push_back(2);
            else                result.push_back(0);
            if ((int)result.size() >= expectedCols) break;
        } else { ++p; }
    }
    return result;
}

// Parse table row cells. Strips trailing empty cells.
static std::vector<std::string> parseTableRow(const char* line,
                                               const char* lineEnd)
{
    std::vector<std::string> cells;
    const char* p = line;
    while (p < lineEnd) {
        if (*p == '|') {
            ++p;
            const char* start = p;
            while (p < lineEnd && *p != '|') ++p;
            while (start < p && *start == ' ') ++start;
            const char* end = p;
            while (end > start && *(end - 1) == ' ') --end;
            cells.emplace_back(start, end);
        } else { ++p; }
    }
    while (!cells.empty() && cells.back().empty())
        cells.pop_back();
    return cells;
}

static bool isTableSep(const char* line, const char* lineEnd)
{
    const char* p = line;
    bool hasPipe = false;
    while (p < lineEnd) {
        if (*p == '|') { hasPipe = true; ++p; continue; }
        if (*p == ' ' || *p == '-' || *p == ':' || *p == '\t')
            { ++p; continue; }
        return false;
    }
    return hasPipe;
}

// ============================================================================
// Main render loop
// ============================================================================

enum State { Normal, CodeBlock, Table };

void renderMarkdown(const std::string& text, float maxWidth,
                    MarkdownLinkCallback onLink)
{
    if (text.empty()) return;

    // Apply internal padding
    float padLeft = PAD_X;
    float availW  = maxWidth - padLeft * 2.0f;

    // Split into lines
    std::vector<const char*> linePtrs;
    std::vector<size_t>      lineLens;
    const char* s = text.c_str();
    const char* end = s + text.size();
    while (s < end) {
        const char* nl = s;
        while (nl < end && *nl != '\n') ++nl;
        linePtrs.push_back(s);
        lineLens.push_back(nl - s);
        s = (nl < end) ? nl + 1 : nl;
    }

    float offsetX = ImGui::GetCursorPosX() + padLeft;

    // Fast path
    if (linePtrs.size() == 1 && lineLens[0] < 200) {
        std::vector<InlineSeg> segs;
        const char* ln = linePtrs[0];
        parseInlineSegs(ln, ln + lineLens[0], segs);
        renderInlineWrapped(segs, offsetX, offsetX + availW, onLink);
        return;
    }

    State state = Normal;
    std::vector<std::string> codeLines;
    TableData                tableData;

    auto flushCode = [&]() {
        if (!codeLines.empty()) {
            renderCodeBlock(codeLines, availW + padLeft);
            codeLines.clear();
        }
    };
    auto flushTable = [&]() {
        if (!tableData.headers.empty()) {
            renderTable(tableData, availW + padLeft);
            tableData = TableData{};
        }
    };

    auto renderLine = [&](const char* text, const char* textEnd,
                           float indentX, float wrapX) -> float {
        std::vector<InlineSeg> segs;
        parseInlineSegs(text, textEnd, segs);
        return renderInlineWrapped(segs, indentX, wrapX, onLink);
    };

    for (size_t i = 0; i < linePtrs.size(); ++i) {
        const char* ln = linePtrs[i];
        const char* le = ln + lineLens[i];

        // ---- CodeBlock ----
        if (state == CodeBlock) {
            const char* cs = skipSpace(ln);
            if (startsWith(cs, "```")) {
                flushCode(); state = Normal; continue;
            }
            codeLines.emplace_back(ln, le);
            continue;
        }

        // ---- Table ----
        if (state == Table) {
            const char* ts = skipSpace(ln);
            if (ts < le && *ts == '|') {
                // Separator row: skip silently
                if (isTableSep(ts, le)) continue;
                // Data row
                auto cells = parseTableRow(ts, le);
                bool any = false;
                for (auto& c : cells)
                    if (!c.empty()) { any = true; break; }
                if (any) {
                    std::vector<std::vector<InlineSeg>> rowSegs;
                    for (auto& cell : cells) {
                        std::vector<InlineSeg> segs;
                        parseInlineSegs(cell.c_str(),
                            cell.c_str() + cell.size(), segs);
                        rowSegs.push_back(std::move(segs));
                    }
                    tableData.rows.push_back(std::move(rowSegs));
                    continue;
                }
            }
            flushTable(); state = Normal;
        }

        // ---- Normal ----
        const char* ls = skipSpace(ln);
        size_t trimmedLen = le - ls;

        // Empty line
        if (trimmedLen == 0) {
            flushCode(); flushTable();
            ImGui::SetCursorScreenPos(ImVec2(offsetX,
                ImGui::GetCursorScreenPos().y + fontSz() * 0.3f));
            ImGui::Dummy(ImVec2(1.0f, 0.0f));
            continue;
        }

        // Code fence
        if (startsWith(ls, "```")) {
            flushTable(); state = CodeBlock; codeLines.clear(); continue;
        }

        // HR
        if (trimmedLen >= 3) {
            char ch = *ls;
            if ((ch == '-' || ch == '*' || ch == '_') &&
                ls[0] == ch && ls[1] == ch && ls[2] == ch) {
                bool allSame = true;
                for (size_t k = 3; k < trimmedLen; ++k)
                    if (ls[k] != ch && ls[k] != ' ') {
                        allSame = false; break;
                    }
                if (allSame) {
                    flushTable();
                    ImGui::SetCursorScreenPos(ImVec2(offsetX,
                        ImGui::GetCursorScreenPos().y + fontSz()*0.2f));
                    ImGui::PushStyleColor(ImGuiCol_Separator, ThemeColors::toVec4(ThemeManager::instance().current().mdHR));
                    ImGui::Separator();
                    ImGui::PopStyleColor();
                    ImGui::SetCursorScreenPos(ImVec2(offsetX,
                        ImGui::GetCursorScreenPos().y + fontSz()*0.2f));
                    ImGui::Dummy(ImVec2(1.0f, 0.0f));
                    continue;
                }
            }
        }

        // Header
        if (*ls == '#') {
            flushTable();
            int level = 0;
            while (ls < le && *ls == '#') { ++level; ++ls; }
            if (ls < le && *ls == ' ' && level <= 6) {
                ++ls;
                float scale = 1.5f - level * 0.15f;
                if (scale < 1.0f) scale = 1.0f;

                ImGui::SetCursorScreenPos(ImVec2(offsetX,
                    ImGui::GetCursorScreenPos().y + fontSz()*0.4f));
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 cp = ImGui::GetCursorScreenPos();
                float hh = fontSz() * scale + fontSz() * 0.4f;
                dl->AddRectFilled(cp, ImVec2(cp.x + availW, cp.y + hh),
                                  IM_COL32(255,255,255,15), 3.0f);

                ImVec4 hc = (level==1) ? ThemeColors::toVec4(ThemeManager::instance().current().mdH1) : (level==2) ? ThemeColors::toVec4(ThemeManager::instance().current().mdH2) : ThemeColors::toVec4(ThemeManager::instance().current().mdH3);
                ImGui::PushStyleColor(ImGuiCol_Text, hc);
                ImGui::SetWindowFontScale(scale);
                ImGui::TextUnformatted(ls, le);
                ImGui::SetWindowFontScale(1.0f);
                ImGui::PopStyleColor();
                ImGui::SetCursorScreenPos(ImVec2(offsetX,
                    ImGui::GetCursorScreenPos().y + fontSz()*0.3f));
                ImGui::Dummy(ImVec2(1.0f, 0.0f));
                continue;
            }
            ls = skipSpace(ln);
        }

        // Blockquote
        if (*ls == '>') {
            flushTable();
            ++ls; if (*ls == ' ') ++ls;
            float qx = offsetX + 4.0f;
            float qw = offsetX + availW - 4.0f;

            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 cp = ImGui::GetCursorScreenPos();
            dl->AddRectFilled(ImVec2(qx - 4.0f, cp.y),
                ImVec2(qx - 1.0f, cp.y + lineH()),
                ImGui::ColorConvertFloat4ToU32(ThemeColors::toVec4(ThemeManager::instance().current().mdQuoteBar)));

            ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::toVec4(ThemeManager::instance().current().mdQuote));
            ImGui::SetCursorScreenPos(ImVec2(qx, cp.y));
            float endY = renderLine(ls, le, qx, qw);
            ImGui::PopStyleColor();
            ImGui::SetCursorScreenPos(ImVec2(offsetX, endY));
            ImGui::Dummy(ImVec2(1.0f, 0.0f));
            continue;
        }

        // Table row
        if (*ls == '|') {
            flushCode();
            if (state != Table) {
                bool looksLikeTable = false;
                if (i + 1 < linePtrs.size()) {
                    const char* nls = skipSpace(linePtrs[i + 1]);
                    const char* nle = linePtrs[i+1] + lineLens[i+1];
                    if (nls < nle && *nls == '|' && isTableSep(nls, nle))
                        looksLikeTable = true;
                }
                if (!looksLikeTable) {
                    ImGui::SetCursorScreenPos(ImVec2(offsetX,
                        ImGui::GetCursorScreenPos().y));
                    renderLine(ls, le, offsetX, offsetX + availW);
                    continue;
                }
                state = Table;
                tableData = TableData{};
                auto rawHdr = parseTableRow(ls, le);
                tableData.headers = rawHdr;
                for (auto& h : rawHdr) {
                    std::vector<InlineSeg> segs;
                    parseInlineSegs(h.c_str(), h.c_str() + h.size(), segs);
                    tableData.headerSegs.push_back(std::move(segs));
                }
                if (i + 1 < linePtrs.size()) {
                    const char* sls = skipSpace(linePtrs[i+1]);
                    const char* sle = linePtrs[i+1] + lineLens[i+1];
                    tableData.align = parseTableAlign(
                        sls, sle, (int)tableData.headers.size());
                }
                continue;
            }
            continue;
        }

        // Bullet: - or * + space
        if ((*ls == '-' || *ls == '*') &&
            ls + 1 < le && *(ls + 1) == ' ') {
            flushTable();
            float bx = offsetX + 4.0f;
            float tx = bx + fontSz() * 0.8f;
            float wx = offsetX + availW;

            float cy = ImGui::GetCursorScreenPos().y;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddCircleFilled(ImVec2(bx + fontSz()*0.3f,
                cy + fontSz()*0.45f), fontSz()*0.18f,
                ImGui::ColorConvertFloat4ToU32(ThemeColors::toVec4(ThemeManager::instance().current().mdBullet)));

            ImGui::SetCursorScreenPos(ImVec2(tx, cy));
            float endY = renderLine(ls + 2, le, tx, wx);
            ImGui::SetCursorScreenPos(ImVec2(offsetX, endY));
            ImGui::Dummy(ImVec2(1.0f, 0.0f));
            continue;
        }

        // Ordered list: N. 
        {
            const char* ns = ls;
            while (ns < le && std::isdigit((unsigned char)*ns)) ++ns;
            if (ns > ls && ns < le && *ns == '.' &&
                ns + 1 < le && *(ns + 1) == ' ') {
                flushTable();
                std::string num(ls, ns + 1);
                float numW = ImGui::CalcTextSize(num.c_str()).x;
                float bx = offsetX + 4.0f;
                float tx = bx + numW + fontSz() * 0.4f;
                float wx = offsetX + availW;

                float cy = ImGui::GetCursorScreenPos().y;
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddText(ImGui::GetFont(), fontSz(), ImVec2(bx, cy),
                    ImGui::ColorConvertFloat4ToU32(ThemeColors::toVec4(ThemeManager::instance().current().mdBullet)), num.c_str());

                ImGui::SetCursorScreenPos(ImVec2(tx, cy));
                float endY = renderLine(ns + 2, le, tx, wx);
                ImGui::SetCursorScreenPos(ImVec2(offsetX, endY));
                ImGui::Dummy(ImVec2(1.0f, 0.0f));
                continue;
            }
        }

        // Paragraph (default)
        {
            flushCode(); flushTable();
            ImGui::SetCursorScreenPos(ImVec2(offsetX,
                ImGui::GetCursorScreenPos().y));
            renderLine(ln, le, offsetX, offsetX + availW);
        }
    }

    flushCode();
    flushTable();
}
