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
    case markdown_text::Style::H1:
        return studio_theme::fromHex(T.mdH1);
    case markdown_text::Style::H2:
        return studio_theme::fromHex(T.mdH2);
    case markdown_text::Style::H3:
        return studio_theme::fromHex(T.mdH3);
    case markdown_text::Style::Bold:
        return studio_theme::fromHex(T.mdBold);
    case markdown_text::Style::Italic:
        return studio_theme::fromHex(T.mdItalic);
    case markdown_text::Style::Code:
    case markdown_text::Style::CodeBlock:
        return studio_theme::fromHex(T.mdCode);
    case markdown_text::Style::Link:
        return studio_theme::fromHex(T.mdLink);
    case markdown_text::Style::Quote:
        return studio_theme::fromHex(T.mdQuote);
    case markdown_text::Style::Bullet:
    case markdown_text::Style::Ordered:
        return studio_theme::fromHex(T.mdBullet);
    // mdTableHdr is a BACKGROUND in this theme (the TUI paints the header row with
    // it). Drawing glyphs in it made the header disappear; the row now gets a band
    // instead (drawBlock) and the text keeps the body colour.
    case markdown_text::Style::TableHeader:
        return studio_theme::fromHex(T.text);
    default:
        return studio_theme::fromHex(T.text);
    }
}

bool allOfStyle(const markdown_text::Line& line, markdown_text::Style style) {
    if (line.segs.empty()) return false;
    for (const auto& s : line.segs)
        if (s.style != style && s.style != markdown_text::Style::Normal) return false;
    return true;
}

// A fenced-code line is ALL CodeBlock, with no tolerance for Normal. The tolerant
// test above is right for a quote or a bullet, whose wrapped runs may carry plain
// segments -- but for code it made every PROSE paragraph a code line: its segs are
// all Normal, and Normal counted as "same style". Each paragraph was therefore
// drawn as a fenced block: a full-width mdCodeBg band and the code hang indent
// (the dark strip behind ordinary sentences in a screenshot).
bool isCodeLine(const markdown_text::Line& line) {
    if (line.segs.empty()) return false;
    for (const auto& s : line.segs)
        if (s.style != markdown_text::Style::CodeBlock) return false;
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
                         (blockA == blockB && (lineA < lineB || (lineA == lineB && colA <= colB)));
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
                                                             float width, int blockId,
                                                             std::vector<TableData>* tables) {
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

        // ---- table: hand the whole thing to ImGui's table widget ----------------
        if (!line.cells.empty()) {
            size_t j = i;
            std::vector<const markdown_text::Line*> rows;
            while (j < lines.size() && !lines[j].cells.empty())
                rows.push_back(&lines[j++]);
            size_t cols = 0;
            for (const auto* r : rows)
                cols = std::max(cols, r->cells.size());

            TableData td;
            td.weights.assign(std::max<size_t>(cols, 1), 1.0f);
            for (const auto* r : rows) {
                std::vector<std::string> cells(std::max<size_t>(cols, 1));
                for (size_t c = 0; c < cols; ++c) {
                    std::string text;
                    if (c < r->cells.size())
                        for (const auto& seg : r->cells[c])
                            text += seg.text;
                    cells[c] = text;
                    td.weights[c] = std::max(td.weights[c], static_cast<float>(columnsOf(text)));
                }
                td.rows.push_back(std::move(cells));
            }
            if (tables) {
                VisualLine vl;
                vl.block = blockId;
                vl.line = lineNo++;
                vl.tableIndex = static_cast<int>(tables->size());
                tables->push_back(std::move(td));
                out.push_back(std::move(vl));
            }
            i = j;
            continue;
        }

        // ---- fenced code: one band, padded ----
        if (isCodeLine(line)) {
            size_t j = i;
            while (j < lines.size() && isCodeLine(lines[j])) {
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
        entry.second.tables.clear();
        entry.second.lines = buildLines(text, markdown, width, blockId, &entry.second.tables);
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
        // ONE decision, used by the pop at the bottom of this loop. tier 1 (###
        // H3) is bold at body size: it pushes no face. Popping on `tier != 0`
        // while pushing only 2 and 3 popped a font that was never pushed, which
        // is the "Calling PopFont() too many times" assert -- once per H3 line.
        const bool facePushed = vl.tier == 3 || vl.tier == 2;
        if (vl.tier == 3)
            studio_theme::pushText(studio_theme::TextRole::Title);
        else if (vl.tier == 2)
            studio_theme::pushText(studio_theme::TextRole::Heading);
        // A markdown table is one widget, not a run of padded text lines: hand the
        // parsed rows to ImGui's table (the GUI's ftxui::Table). It brings its own
        // borders, header band and zebra, all from the theme's ImGuiCol_Table* entries.
        if (vl.tableIndex >= 0) {
            if (vl.tableIndex < static_cast<int>(layout->tables.size())) {
                // NOT plainColor directly: a markdown body passes 0 there (transparent)
                // because its runs are coloured per style. Pushing it made the whole
                // table invisible -- runColor() resolves 0 to the theme's text colour.
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      runColor(markdown_text::Style::Normal, plainColor));
                drawTable(layout->tables[static_cast<size_t>(vl.tableIndex)]);
                ImGui::PopStyleColor();
            }
            // Keep the line's box so hit-testing and the selection still know where it
            // is; without this it would sit at (0,0) and swallow clicks in the corner.
            const ImVec2 p0 = ImGui::GetItemRectMin();
            const ImVec2 p1 = ImGui::GetItemRectMax();
            vl.x0 = p0.x;
            vl.y0 = p0.y;
            vl.x1 = std::max(p1.x, p0.x + 1.0f);
            vl.y1 = std::max(p1.y, p0.y + 1.0f);
            frameLines_.push_back(vl);
            // This branch leaves the loop early, so it must unwind the face push
            // itself: a heading that turned out to be a table line would otherwise
            // leak one font onto the stack for every frame it stayed on screen.
            if (facePushed) studio_theme::popText();
            if (ind > 0.0f) ImGui::Unindent(ind); // same early exit, same unwind
            continue;
        }

        vl.x0 = ImGui::GetCursorScreenPos().x;
        vl.y0 = ImGui::GetCursorScreenPos().y;
        // Hard clip to this line's own box. The wrap decides where a line ends, but a
        // single glyph must never be painted outside the surface it belongs to: the
        // bubble is a card, and text crossing its edge is the one thing a reader
        // notices before anything else.
        dl->PushClipRect(
            ImVec2(vl.x0 - 2.0f, vl.y0 - 2.0f),
            ImVec2(vl.x0 + width + 2.0f * dpi, vl.y0 + ImGui::GetTextLineHeight() + 4.0f * dpi),
            true);

        // Block decoration, painted before the glyphs so it sits behind them.
        if (vl.codeBg) {
            const float pad = 5.0f * dpi;
            dl->AddRectFilled(
                ImVec2(vl.x0 - pad, vl.y0 - pad * 0.4f),
                ImVec2(vl.x0 + width - ind + pad, vl.y0 + ImGui::GetTextLineHeight() + pad * 0.4f),
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
                dl->AddRectFilled(ImVec2(p.x - 3.0f, p.y - 1.0f),
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
        frameLines_.push_back(vl);
        if (facePushed) studio_theme::popText();
        if (ind > 0.0f) ImGui::Unindent(ind);
    }
}

void TextSurface::drawTable(const TableData& t) {
    if (t.rows.empty() || t.rows[0].empty()) return;
    const int cols = static_cast<int>(t.rows[0].size());
    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX;
    ImGui::PushID(&t);
    if (ImGui::BeginTable("##mdtable", cols, flags)) {
        float total = 0.0f;
        for (float w : t.weights)
            total += std::max(1.0f, w);
        if (total <= 0.0f) total = static_cast<float>(cols);
        for (int c = 0; c < cols; ++c) {
            // The weight is the column's natural width in character cells, so a 2-glyph
            // "项" column stays narrow and the long text column takes the rest. ImGui's
            // own stretch then shares out whatever the bubble gives the table.
            const float w = c < static_cast<int>(t.weights.size())
                                ? std::max(1.0f, t.weights[static_cast<size_t>(c)])
                                : 1.0f;
            ImGui::TableSetupColumn(t.rows[0][static_cast<size_t>(c)].c_str(),
                                    ImGuiTableColumnFlags_WidthStretch, w / total);
        }
        ImGui::TableHeadersRow(); // the band comes from ImGuiCol_TableHeaderBg (theme)
        const std::string empty;
        for (size_t r = 1; r < t.rows.size(); ++r) {
            ImGui::TableNextRow();
            for (int c = 0; c < cols; ++c) {
                ImGui::TableSetColumnIndex(c);
                const std::string& cell = c < static_cast<int>(t.rows[r].size())
                                              ? t.rows[r][static_cast<size_t>(c)]
                                              : empty;
                ImGui::TextWrapped("%s", cell.c_str()); // wraps inside its own cell
            }
        }
        ImGui::EndTable();
    }
    ImGui::PopID();
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
                    while (e < vl.plain.size() && vl.plain[e] != ' ')
                        e += cpLen(vl.plain, e);
                    size_t s = b;
                    while (s > 0 && vl.plain[s - 1] != ' ')
                        s -= cpLen(vl.plain, s - 1);
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
