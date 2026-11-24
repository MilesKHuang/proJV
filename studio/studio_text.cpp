// proJV studio -- selectable, cached markdown layout (see studio_text.h).
//
// Rewritten as a BLOCK engine rather than a run flattener, because the flattened
// version had the tell-tale look of a terminal pasted into a window: a wrapped
// bullet lost its hanging indent (its second line started at the window edge), a
// quote had no bar, a code block had no padding, a heading could only be a colour.
//
// Layout is separated from drawing: buildLines() decides shape (indent, tier,
// decoration), drawBlock() only submits items. The cache key is
// (blockId, width, content hash), so a settled message is parsed once.
#include "studio_text.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "debug_log.h"
#include "design/tokens.h"
#include "studio_markdown.h"
#include "studio_theme.h"
#include "tui/theme_manager.h"

namespace studio {
namespace {

size_t hashText(const std::string& s) {
    size_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

float charWidth() {
    return ImGui::CalcTextSize("M").x;
}

int codepointWidth(const std::string& s, size_t i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    return c >= 0xE0 ? 2 : 1; // CJK / full-width shapes occupy two columns
}

size_t cpLen(const std::string& s, size_t i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c >= 0xF0) return 4;
    if (c >= 0xE0) return 3;
    if (c >= 0xC0) return 2;
    return 1;
}

int columnsOf(const std::string& s) {
    int cols = 0;
    size_t i = 0;
    while (i < s.size()) {
        cols += codepointWidth(s, i);
        i += cpLen(s, i);
    }
    return cols;
}

ImU32 runColor(markdown_text::Style style, ImU32 plainOverride) {
    if (plainOverride) return plainOverride;
    const ThemeColors& T = ThemeManager::instance().current();
    switch (style) {
    case markdown_text::Style::H1: return studio_theme::fromHex(T.mdH1);
    case markdown_text::Style::H2: return studio_theme::fromHex(T.mdH2);
    case markdown_text::Style::H3: return studio_theme::fromHex(T.mdH3);
    case markdown_text::Style::Bold: return studio_theme::fromHex(T.mdBold);
    case markdown_text::Style::Italic: return studio_theme::fromHex(T.mdItalic);
    case markdown_text::Style::Code:
    case markdown_text::Style::CodeBlock: return studio_theme::fromHex(T.mdCode);
    case markdown_text::Style::Link: return studio_theme::fromHex(T.mdLink);
    case markdown_text::Style::Quote: return studio_theme::fromHex(T.mdQuote);
    case markdown_text::Style::Bullet:
    case markdown_text::Style::Ordered: return studio_theme::fromHex(T.mdBullet);
    // mdTableHdr is a BACKGROUND in this theme (the TUI paints the header row with
    // it). Drawing glyphs in it made the header disappear; the row now gets a band
    // instead (drawBlock) and the text keeps the body colour.
    case markdown_text::Style::TableHeader: return studio_theme::fromHex(T.text);
    default: return studio_theme::fromHex(T.text);
    }
}

bool allOfStyle(const markdown_text::Line& line, markdown_text::Style style) {
    if (line.segs.empty()) return false;
    for (const auto& s : line.segs)
        if (s.style != style && s.style != markdown_text::Style::Normal) return false;
    return true;
}

int tierOf(const markdown_text::Line& line) {
    if (line.segs.empty()) return 0;
    const auto st = line.segs[0].style;
    if (st == markdown_text::Style::H3) return 1;
    if (st == markdown_text::Style::H2) return 2;
    if (st == markdown_text::Style::H1) return 3;
    return 0;
}

} // namespace

void Selection::ordered(int* b0, int* l0, int* c0, int* b1, int* l1, int* c1) const {
    const bool forward = (blockA < blockB) ||
                         (blockA == blockB &&
                          (lineA < lineB || (lineA == lineB && colA <= colB)));
    *b0 = forward ? blockA : blockB;
    *l0 = forward ? lineA : lineB;
    *c0 = forward ? colA : colB;
    *b1 = forward ? blockB : blockA;
    *l1 = forward ? lineB : lineA;
    *c1 = forward ? colB : colA;
}

int TextSurface::columnAtX(const VisualLine& line, float x) {
    const float cw = charWidth() > 0.0f ? charWidth() : 8.0f;
    const int target = static_cast<int>((x - line.x0) / cw + 0.5f);
    if (target <= 0) return 0;
    int cols = 0;
    size_t i = 0;
    while (i < line.plain.size()) {
        const int w = codepointWidth(line.plain, i);
        if (cols + w > target) return cols;
        cols += w;
        i += cpLen(line.plain, i);
    }
    return cols;
}

std::string TextSurface::textInLine(const VisualLine& line, int c0, int c1) {
    if (c0 < 0) c0 = 0;
    if (c1 < c0) c1 = c0;
    std::string out;
    int cols = 0;
    size_t i = 0;
    while (i < line.plain.size()) {
        const int w = codepointWidth(line.plain, i);
        const size_t len = cpLen(line.plain, i);
        if (cols >= c0 && cols < c1) out.append(line.plain, i, len);
        cols += w;
        i += len;
    }
    return out;
}

std::vector<TextSurface::VisualLine> TextSurface::buildLines(const std::string& text, bool markdown,
                                                             float width, int blockId) {
    std::vector<VisualLine> out;
    int lineNo = 0;
    const float cw = charWidth() > 0.0f ? charWidth() : 8.0f;
    const float bulletHang = cw * 2.5f; // marker column + one space
    const float quoteHang = cw * 1.5f;
    const float codeHang = cw * 1.0f;
    // The three tier sizes the draw path can push, from the one token ladder.
    const float bodyPx = tokens::kTypePx[static_cast<int>(tokens::Type::Body)];
    const float titlePx = tokens::kTypePx[static_cast<int>(tokens::Type::Title)];
    const float headingPx = tokens::kTypePx[static_cast<int>(tokens::Type::Heading)];

    const auto emitWrapped = [&](const std::vector<markdown_text::Segment>& segs, float hang,
                                 int tier, bool codeBg, bool quote, bool rule,
                                 std::vector<markdown_text::Segment> prefix) {
        // A heading is WRAPPED here but DRAWN at its own size (Title 22 / Heading 18
        // against a 16 px body). Without scaling the wrap width by the same ratio the
        // line was fitted to a face it is not painted in, and the text ran past the
        // bubble: that is the "text spills out of the bubble" report. Ratios come from
        // the token ladder, so the two sizes cannot drift.
        const float tierScale = (tier == 3)   ? (bodyPx / titlePx)
                                : (tier == 2) ? (bodyPx / headingPx)
                                              : 1.0f;
        const float avail = std::max(16.0f, (width - hang) * tierScale);
        auto rows = studio_md::wrap(segs, avail);
        for (size_t r = 0; r < rows.size(); ++r) {
            VisualLine vl;
            vl.block = blockId;
            vl.line = lineNo++;
            vl.tier = tier;
            vl.rule = rule;
            vl.quote = quote;
            vl.codeBg = codeBg;
            vl.indent = (r == 0) ? 0.0f : hang;
            if (r == 0) {
                for (auto& p : prefix) {
                    vl.plain += p.text;
                    vl.runs.push_back(std::move(p));
                }
            }
            for (const auto& s : rows[r]) {
                vl.plain += s.text;
                vl.runs.push_back(s);
            }
            out.push_back(std::move(vl));
        }
    };

    if (!markdown) {
        // Plain text keeps its own line breaks. Passing the whole block as ONE segment
        // meant every '\n' travelled inside a single "line", and ImGui rendered it as
        // one tall row with stray gaps -- the "thinking body has random blank lines"
        // report.
        size_t pos = 0;
        while (pos <= text.size()) {
            size_t nl = text.find('\n', pos);
            const bool last = (nl == std::string::npos);
            const std::string row = text.substr(pos, last ? std::string::npos : nl - pos);
            if (!row.empty()) {
                markdown_text::Segment seg;
                seg.text = row;
                seg.style = markdown_text::Style::Normal;
                emitWrapped({seg}, 0.0f, 0, false, false, false, {});
            } else {
                VisualLine vl;
                vl.block = blockId;
                vl.line = lineNo++;
                out.push_back(std::move(vl));
            }
            if (last) break;
            pos = nl + 1;
        }
        if (out.empty()) out.push_back(VisualLine{});
        return out;
    }

    const auto lines = markdown_text::parseMarkdown(text);
    size_t i = 0;
    while (i < lines.size()) {
        const auto& line = lines[i];

        // ---- table: a run of rows carrying cells instead of segments ----
        if (!line.cells.empty()) {
            size_t j = i;
            std::vector<const markdown_text::Line*> rows;
            while (j < lines.size() && !lines[j].cells.empty()) rows.push_back(&lines[j++]);
            size_t cols = 0;
            for (const auto* r : rows) cols = std::max(cols, r->cells.size());
            const auto cellText = [](const markdown_text::Line& l, size_t c) {
                std::string t;
                if (c < l.cells.size())
                    for (const auto& s : l.cells[c]) t += s.text;
                return t;
            };
            std::vector<int> widths(cols, 0);
            for (const auto* r : rows)
                for (size_t c = 0; c < cols; ++c)
                    widths[c] = std::max(widths[c], columnsOf(cellText(*r, c)));
            // Fit the table into the bubble instead of throwing its right-hand columns
            // away. The old rule kept the first columns that fitted and silently
            // dropped the rest, so a two-column table whose second column was long
            // showed nothing but the first one -- the "table shows only the SHA" report.
            // Every column now gets a share of the width and wraps inside it.
            const int maxCols = std::max(8, static_cast<int>(width / std::max(1.0f, cw)));
            const int seps = 3 * (static_cast<int>(cols) - 1) + 4; // separators + the │ frame
            int natural = seps;
            for (int w : widths) natural += std::max(w, 1);
            if (natural > maxCols && cols > 0) {
                const std::vector<int> need = widths;
                const int avail = std::max<int>(static_cast<int>(cols) * 6, maxCols - seps);
                // Every narrow column keeps its content -- a 7-character hash must not be
                // cut down to 4 because the column beside it is long. The wide columns
                // share what is left, and a remainder flows back to them in order.
                const int share = std::max(8, avail / static_cast<int>(cols));
                int left = avail;
                for (size_t c = 0; c < cols; ++c) {
                    widths[c] = std::min(need[c], share);
                    left -= widths[c];
                }
                for (size_t c = 0; c < cols && left > 0; ++c) {
                    const int extra = std::min(left, need[c] - widths[c]);
                    widths[c] += extra;
                    left -= extra;
                }
                for (size_t c = cols; left < 0 && c-- > 0;) {
                    const int cut = std::min(-left, std::max(0, widths[c] - 4));
                    widths[c] -= cut;
                    left += cut;
                }
            }
            for (size_t ri = 0; ri < rows.size(); ++ri) {
                const markdown_text::Line* r = rows[ri];
                // Wrap each cell into its own column width, then emit one visual line
                // per line of the tallest cell: a long cell grows downwards instead of
                // running past the bubble.
                std::vector<std::vector<std::vector<markdown_text::Segment>>> cellRows(cols);
                size_t rowLines = 1;
                for (size_t c = 0; c < cols; ++c) {
                    std::vector<markdown_text::Segment> segs;
                    if (c < r->cells.size()) segs = r->cells[c];
                    const float colPx = static_cast<float>(std::max(1, widths[c])) * cw;
                    cellRows[c] = studio_md::wrap(segs, colPx);
                    if (cellRows[c].empty()) cellRows[c].push_back({});
                    rowLines = std::max(rowLines, cellRows[c].size());
                }
                for (size_t k = 0; k < rowLines; ++k) {
                    // UTF-8 bytes, not a universal character name: without /utf-8 MSVC
                    // would encode │ in the system code page.
                    std::string text = "\xE2\x94\x82 "; // left edge
                    for (size_t c = 0; c < cols; ++c) {
                        std::string cell;
                        if (k < cellRows[c].size())
                            for (const auto& seg : cellRows[c][k]) cell += seg.text;
                        int have = columnsOf(cell);
                        while (have < widths[c]) {
                            cell += ' ';
                            ++have;
                        }
                        text += cell;
                        text += (c + 1 < cols) ? " \xE2\x94\x82 " : " \xE2\x94\x82";
                    }
                    VisualLine vl;
                    vl.block = blockId;
                    vl.line = lineNo++;
                    vl.table = true;
                    vl.tableTop = (ri == 0 && k == 0);
                    vl.tableLast = (ri + 1 == rows.size() && k + 1 == rowLines);
                    vl.tableHeaderRow = r->tableHeader;
                    markdown_text::Segment s;
                    s.text = text;
                    s.style = r->tableHeader ? markdown_text::Style::TableHeader
                                             : markdown_text::Style::TableCell;
                    vl.plain = text;
                    vl.runs.push_back(std::move(s));
                    // One rule under every row: that is the grid the TUI draws, and it
                    // is what turns a run of padded text into something with cells.
                    if (k + 1 == rowLines) vl.rule = true;
                    out.push_back(std::move(vl));
                }
            }
            i = j;
            continue;
        }

        // ---- fenced code: one band, padded ----
        if (allOfStyle(line, markdown_text::Style::CodeBlock)) {
            size_t j = i;
            while (j < lines.size() && allOfStyle(lines[j], markdown_text::Style::CodeBlock)) {
                VisualLine vl;
                vl.block = blockId;
                vl.line = lineNo++;
                vl.codeBg = true;
                vl.indent = codeHang;
                for (const auto& s : lines[j].segs) {
                    vl.plain += s.text;
                    vl.runs.push_back(s);
                }
                out.push_back(std::move(vl));
                ++j;
            }
            i = j;
            continue;
        }

        if (line.segs.empty()) {
            VisualLine vl;
            vl.block = blockId;
            vl.line = lineNo++;
            out.push_back(std::move(vl));
            ++i;
            continue;
        }

        const int tier = tierOf(line);
        if (tier != 0) {
            emitWrapped(line.segs, 0.0f, tier, false, false, false, {});
            ++i;
            continue;
        }

        if (line.segs[0].style == markdown_text::Style::Quote) {
            emitWrapped(line.segs, quoteHang, 0, false, /*quote=*/true, false, {});
            ++i;
            continue;
        }

        // ---- bullet: the tokenizer stripped "- ", so put a marker back and hang
        //      wrapped text under the TEXT, not under the marker ----
        if (line.segs[0].style == markdown_text::Style::Bullet) {
            markdown_text::Segment dot;
            dot.text = "\xe2\x80\xa2 "; // U+2022 + space
            dot.style = markdown_text::Style::Bullet;
            emitWrapped(line.segs, bulletHang, 0, false, false, false, {dot});
            ++i;
            continue;
        }

        // ---- ordered item: the number is already a segment; the hang accounts for
        //      its width so wrapped text stays aligned ----
        if (line.segs[0].style == markdown_text::Style::Ordered) {
            const std::string num = line.segs[0].text;
            std::vector<markdown_text::Segment> body(line.segs.begin() + 1, line.segs.end());
            const float hang = cw * static_cast<float>(columnsOf(num) + 1);
            emitWrapped(body, hang, 0, false, false, false, {line.segs[0]});
            ++i;
            continue;
        }

        // ---- horizontal rule ----
        if (line.segs.size() == 1 && line.segs[0].style == markdown_text::Style::HR) {
            VisualLine vl;
            vl.block = blockId;
            vl.line = lineNo++;
            vl.rule = true;
            out.push_back(std::move(vl));
            ++i;
            continue;
        }

        emitWrapped(line.segs, 0.0f, 0, false, false, false, {});
        ++i;
    }
    if (out.empty()) out.push_back(VisualLine{});
    return out;
}

const TextSurface::Cached* TextSurface::cachedLayout(int blockId, const std::string& text,
                                                     bool markdown, float width) {
    const size_t h = hashText(text);
    for (auto& entry : cache_) {
        if (entry.first != blockId) continue;
        if (entry.second.hash == h && entry.second.width == width &&
            entry.second.markdown == markdown) {
            return &entry.second; // unchanged: no parse, no wrap
        }
        entry.second.hash = h;
        entry.second.width = width;
        entry.second.markdown = markdown;
        entry.second.lines = buildLines(text, markdown, width, blockId);
        return &entry.second;
    }
    cache_.emplace_back(blockId, Cached{});
    return cachedLayout(blockId, text, markdown, width);
}

void TextSurface::drawBlock(int blockId, const std::string& text, bool markdown, float width,
                            ImU32 plainColor) {
    if (text.empty()) return;
    const Cached* layout = cachedLayout(blockId, text, markdown, width);
    if (!layout) return;
    const float dpi = ImGui::GetFontSize() / 18.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    for (const auto& cachedLine : layout->lines) {
        VisualLine vl = cachedLine;
        // Air above a heading: glued to the paragraph above it, a heading reads as
        // part of that paragraph.
        if (vl.tier != 0 && &cachedLine != &layout->lines.front()) {
            const float air = (vl.tier == 1 ? 10.0f : (vl.tier == 2 ? 14.0f : 18.0f)) * dpi;
            ImGui::Dummy(ImVec2(0.0f, air));
        }
        const float ind = vl.indent;
        if (ind > 0.0f) ImGui::Indent(ind);
        if (vl.tier == 3)
            studio_theme::pushText(studio_theme::TextRole::Title);
        else if (vl.tier == 2)
            studio_theme::pushText(studio_theme::TextRole::Heading);
        // A table row is painted in the monospace face: its cells are padded by
        // character cells, so only a fixed-width face keeps the columns and the │
        // separators lined up (the proportional face turned them into a word salad).
        if (vl.table) studio_theme::pushMono(studio_theme::TextRole::Body);

        vl.x0 = ImGui::GetCursorScreenPos().x;
        vl.y0 = ImGui::GetCursorScreenPos().y;
        // The table's top edge. The bottom edge is drawn with the other rules, after
        // the last row has been painted and its height is known.
        if (vl.tableTop)
            dl->AddLine(ImVec2(vl.x0, vl.y0 - 3.0f * dpi),
                        ImVec2(vl.x0 + width - ind, vl.y0 - 3.0f * dpi),
                        studio_theme::field(&ThemeColors::separator));
        // The header row sits on a band. mdTableHdr is a BACKGROUND colour in this
        // theme (the TUI paints the row with it), which is why it cannot be the text
        // colour -- drawn as glyphs it was invisible.
        if (vl.tableHeaderRow) {
            const float pad = 3.0f * dpi;
            dl->AddRectFilled(ImVec2(vl.x0 - pad, vl.y0 - pad * 0.5f),
                              ImVec2(vl.x0 + width - ind + pad,
                                     vl.y0 + ImGui::GetTextLineHeight() + pad * 0.5f),
                              studio_theme::field(&ThemeColors::mdTableHdr), 3.0f);
        }
        // Hard clip to this line's own box. The wrap decides where a line ends, but a
        // single glyph must never be painted outside the surface it belongs to: the
        // bubble is a card, and text crossing its edge is the one thing a reader
        // notices before anything else.
        dl->PushClipRect(ImVec2(vl.x0 - 2.0f, vl.y0 - 2.0f),
                         ImVec2(vl.x0 + width + 2.0f * dpi,
                                vl.y0 + ImGui::GetTextLineHeight() + 4.0f * dpi),
                         true);

        // Block decoration, painted before the glyphs so it sits behind them.
        if (vl.codeBg) {
            const float pad = 5.0f * dpi;
            dl->AddRectFilled(ImVec2(vl.x0 - pad, vl.y0 - pad * 0.4f),
                              ImVec2(vl.x0 + width - ind + pad,
                                     vl.y0 + ImGui::GetTextLineHeight() + pad * 0.4f),
                              studio_theme::field(&ThemeColors::mdCodeBg), 3.0f);
        }
        if (vl.quote)
            dl->AddRectFilled(ImVec2(vl.x0 - 8.0f * dpi, vl.y0),
                              ImVec2(vl.x0 - 5.0f * dpi, vl.y0 + ImGui::GetTextLineHeight()),
                              studio_theme::field(&ThemeColors::mdQuoteBar));

        const bool bold = vl.tier != 0;
        for (size_t r = 0; r < vl.runs.size(); ++r) {
            if (r > 0) ImGui::SameLine(0.0f, 0.0f);
            const auto style = vl.runs[r].style;
            // Inline `code` needs its own chip: a differently coloured run in a
            // monospace face is not a strong enough signal on its own.
            if (style == markdown_text::Style::Code) {
                const float w = ImGui::CalcTextSize(vl.runs[r].text.c_str()).x;
                const ImVec2 p = ImGui::GetCursorScreenPos();
                dl->AddRectFilled(
                    ImVec2(p.x - 3.0f, p.y - 1.0f),
                    ImVec2(p.x + w + 3.0f, p.y + ImGui::GetTextLineHeight() + 1.0f),
                    studio_theme::field(&ThemeColors::mdCodeBg), 4.0f);
            }
            ImGui::PushStyleColor(ImGuiCol_Text, runColor(style, plainColor));
            ImGui::TextUnformatted(vl.runs[r].text.c_str());
            if (bold || style == markdown_text::Style::Bold) {
                const ImVec2 p = ImGui::GetItemRectMin();
                dl->AddText(ImVec2(p.x + 1.0f, p.y), ImGui::GetColorU32(ImGuiCol_Text),
                            vl.runs[r].text.c_str());
            }
            ImGui::PopStyleColor();
        }
        if (vl.runs.empty()) ImGui::NewLine();
        if (vl.table) studio_theme::popText();

        vl.x1 = ImGui::GetItemRectMax().x;
        vl.y1 = ImGui::GetItemRectMax().y;
        dl->PopClipRect();
        if (vl.x1 <= vl.x0) vl.x1 = vl.x0 + 1.0f; // an empty line still occupies a cell
        // Diagnostic for the "text spills out of the bubble" class of bug: a line that
        // ended up wider than the box it was laid out for. Off unless the layout probe
        // is on, so a normal run pays one comparison per line.
        if (std::getenv("PROJV_STUDIO_DEBUG_LAYOUT") && vl.x1 - vl.x0 > width + 1.0f) {
            debugLogWarnf("[studio] wrapped line overflows its box by %.1f px (block %d line %d)",
                          static_cast<double>((vl.x1 - vl.x0) - width), vl.block, vl.line);
        }
        if (vl.rule) {
            dl->AddLine(ImVec2(vl.x0, vl.y1 + 3.0f * dpi),
                        ImVec2(vl.x0 + width - ind, vl.y1 + 3.0f * dpi),
                        studio_theme::field(&ThemeColors::separator));
        }
        if (vl.tableLast) {
            dl->AddLine(ImVec2(vl.x0, vl.y1 + 3.0f * dpi),
                        ImVec2(vl.x0 + width - ind, vl.y1 + 3.0f * dpi),
                        studio_theme::field(&ThemeColors::separator));
        }
        frameLines_.push_back(vl);
        if (vl.tier != 0) studio_theme::popText();
        if (ind > 0.0f) ImGui::Unindent(ind);
    }
}

bool TextSurface::hitTest(float x, float y, int* block, int* line, int* col) const {
    for (const auto& vl : frameLines_) {
        if (y >= vl.y0 && y <= vl.y1 + 2.0f && x >= vl.x0 - 2.0f && x <= vl.x1 + 40.0f) {
            *block = vl.block;
            *line = vl.line;
            *col = columnAtX(vl, x);
            return true;
        }
    }
    return false;
}

void TextSurface::handleInput() {
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;

    if (ImGui::IsMouseClicked(0)) {
        int block = 0, line = 0, col = 0;
        if (hitTest(mouse.x, mouse.y, &block, &line, &col)) {
            sel_.blockA = sel_.blockB = block;
            sel_.lineA = sel_.lineB = line;
            sel_.colA = sel_.colB = col;
            sel_.active = true;
            dragging_ = true;
            if (ImGui::IsMouseDoubleClicked(0)) {
                for (const auto& vl : frameLines_) {
                    if (vl.block != block || vl.line != line) continue;
                    size_t b = 0;
                    int cols = 0;
                    while (b < vl.plain.size() && cols + codepointWidth(vl.plain, b) <= col) {
                        cols += codepointWidth(vl.plain, b);
                        b += cpLen(vl.plain, b);
                    }
                    size_t e = b;
                    while (e < vl.plain.size() && vl.plain[e] != ' ') e += cpLen(vl.plain, e);
                    size_t s = b;
                    while (s > 0 && vl.plain[s - 1] != ' ') s -= cpLen(vl.plain, s - 1);
                    sel_.colA = columnsOf(vl.plain.substr(0, s));
                    sel_.colB = columnsOf(vl.plain.substr(0, e));
                    break;
                }
            }
        } else {
            sel_.clear();
        }
    }
    if (dragging_ && ImGui::IsMouseDown(0)) {
        int block = 0, line = 0, col = 0;
        if (hitTest(mouse.x, mouse.y, &block, &line, &col)) {
            sel_.blockB = block;
            sel_.lineB = line;
            sel_.colB = col;
        }
    }
    if (ImGui::IsMouseReleased(0)) {
        dragging_ = false;
        if (sel_.blockA == sel_.blockB && sel_.lineA == sel_.lineB && sel_.colA == sel_.colB)
            sel_.clear();
    }

    if (sel_.active && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) {
        const std::string text = selectedText();
        if (!text.empty()) ImGui::SetClipboardText(text.c_str());
    }

    if (sel_.active) {
        int b0, l0, c0, b1, l1, c1;
        sel_.ordered(&b0, &l0, &c0, &b1, &l1, &c1);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float cw = charWidth();
        const ImU32 hi = studio_theme::field(&ThemeColors::buttonActive, 0.45f);
        for (const auto& vl : frameLines_) {
            const bool afterStart = vl.block > b0 || (vl.block == b0 && vl.line >= l0);
            const bool beforeEnd = vl.block < b1 || (vl.block == b1 && vl.line <= l1);
            if (!afterStart || !beforeEnd) continue;
            const int from = (vl.block == b0 && vl.line == l0) ? c0 : 0;
            const int to = (vl.block == b1 && vl.line == l1) ? c1 : columnsOf(vl.plain);
            if (to <= from) continue;
            dl->AddRectFilled(ImVec2(vl.x0 + static_cast<float>(from) * cw, vl.y0),
                              ImVec2(vl.x0 + static_cast<float>(to) * cw, vl.y1), hi);
        }
    }

    frameLines_.clear(); // hit boxes are per frame
}

std::string TextSurface::selectedText() const {
    if (!sel_.active || sel_.empty()) return std::string();
    int b0, l0, c0, b1, l1, c1;
    sel_.ordered(&b0, &l0, &c0, &b1, &l1, &c1);
    std::string out;
    // frameLines_ is cleared at the end of handleInput, so walk the cache instead.
    for (const auto& entry : cache_) {
        for (const auto& vl : entry.second.lines) {
            const bool afterStart = vl.block > b0 || (vl.block == b0 && vl.line >= l0);
            const bool beforeEnd = vl.block < b1 || (vl.block == b1 && vl.line <= l1);
            if (!afterStart || !beforeEnd) continue;
            const int from = (vl.block == b0 && vl.line == l0) ? c0 : 0;
            const int to = (vl.block == b1 && vl.line == l1) ? c1 : columnsOf(vl.plain);
            if (to <= from) continue;
            if (!out.empty()) out += '\n';
            out += textInLine(vl, from, to);
        }
    }
    return out;
}

} // namespace studio
