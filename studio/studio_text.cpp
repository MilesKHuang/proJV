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

// A fenced-code line is ALL CodeBlock, with no tolerance for Normal. Tolerating
// Normal here (as an earlier helper did) made every PROSE paragraph a code line --
// its segs are all Normal, and Normal counted as "same style", so each paragraph
// was drawn as a fenced block: a full-width mdCodeBg band plus the code hang
// indent (the dark strip behind ordinary sentences in a screenshot).
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

int TextSurface::columnAtX(const VisualLine& line, float x) {
    // The line's OWN character width, recorded when it was wrapped. Asking the
    // current font here was wrong twice: handleInput() runs after the chat face has
    // been popped, so it measured the proportional UI face, and a heading pushed yet
    // another size. One ruler per line, or a click lands where the pointer is not.
    const float cw = line.cw > 0.0f ? line.cw : 8.0f;
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

    const auto emitWrapped = [&](const std::vector<markdown_text::Segment>& segs, float hang,
                                 int tier, bool codeBg, bool quote, bool rule,
                                 std::vector<markdown_text::Segment> prefix) {
        // One face, one size, one ruler: a heading is bold plus a theme colour at body
        // size (exactly what the TUI does), so the width it is wrapped to is the width
        // it is painted at. The tier used to shrink this width by bodyPx/titlePx to
        // compensate for a 22 px Title face; with that face gone, keeping the ratio
        // would fit every heading into 73 % of its line and leave a ragged block.
        const float avail = std::max(16.0f, width - hang);
        auto rows = studio_md::wrap(segs, avail);
        for (size_t r = 0; r < rows.size(); ++r) {
            VisualLine vl;
            vl.block = blockId;
            vl.line = lineNo++;
            vl.tier = tier;
            vl.rule = rule;
            vl.quote = quote;
            vl.codeBg = codeBg;
            vl.cw = cw; // the ruler hit-testing and the highlight must reuse
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
        // part of that paragraph. ONE value, not one per tier: a heading is no longer
        // a bigger font (see below), so a tier-dependent gap would still make H1 and H3
        // look like different sizes -- it is the gap that said "bigger", and it would
        // now be the only thing left saying it.
        if (vl.tier != 0 && &cachedLine != &layout->lines.front())
            ImGui::Dummy(ImVec2(0.0f, 8.0f * dpi));
        const float ind = vl.indent;
        if (ind > 0.0f) ImGui::Indent(ind);
        // No face is pushed here on purpose. A markdown heading is body-size, bold and
        // in the theme's mdH1/mdH2/mdH3 colour -- the same rule the TUI follows
        // (markdown_view.cpp pushes bold | color, never a size). It also keeps the ONE
        // ruler this file depends on: buildLines wrapped the line in the mono face at
        // body size, so painting it in another face would put the glyphs somewhere the
        // wrap, the hit-test and the highlight do not expect. The old push (Title 22 /
        // Heading 18, on the PROPORTIONAL face) did exactly that, and its paired pop was
        // the "Calling PopFont() too many times" assert whenever a branch left the loop
        // early.
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
            if (ind > 0.0f) ImGui::Unindent(ind); // early exit, unwind the indent
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

void TextSurface::setClip(float x0, float y0, float x1, float y1) {
    hasClip_ = true;
    clipX0_ = x0;
    clipY0_ = y0;
    clipX1_ = x1;
    clipY1_ = y1;
}

bool TextSurface::snapToNearest(float x, float y, int* block, int* line, int* col) const {
    // The rule: a drag covers every line it passes over, tool rows included. They are
    // visible text in the transcript, and a selection that silently skipped them would
    // copy a conversation that reads as if the tools never ran -- which is a lie about
    // what the agent did, not a convenience.
    const VisualLine* best = nullptr;
    float bestDy = 0.0f;
    for (const auto& vl : frameLines_) {
        const float dy = y < vl.y0 ? vl.y0 - y : (y > vl.y1 ? y - vl.y1 : 0.0f);
        if (!best || dy < bestDy) {
            best = &vl;
            bestDy = dy;
        }
    }
    if (!best) return false;
    *block = best->block;
    *line = best->line;
    // Past a line's right edge the caret belongs at its end, before its left edge at
    // its start -- not "the column the arithmetic happens to produce", which would be
    // negative and clamp to 0 in both cases.
    if (x <= best->x0)
        *col = 0;
    else if (x >= best->x1)
        *col = columnsOf(best->plain);
    else
        *col = columnAtX(*best, x);
    return true;
}

void TextSurface::handleInput(bool interactive) {
    // A modal owns the input while it is open. No click may start a selection behind
    // it, and the highlight must not be painted over it: it goes on the FOREGROUND
    // draw list, which is above every window by definition. The boxes are still
    // cleared, because they are per frame and would otherwise go stale.
    if (!interactive) {
        frameLines_.clear();
        return;
    }
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
            dragging_ = false; // a press outside the text must not extend an old drag
        }
    }
    if (dragging_ && ImGui::IsMouseDown(0)) {
        int block = 0, line = 0, col = 0;
        if (hitTest(mouse.x, mouse.y, &block, &line, &col)) {
            sel_.blockB = block;
            sel_.lineB = line;
            sel_.colB = col;
        } else if (snapToNearest(mouse.x, mouse.y, &block, &line, &col)) {
            // Dragging through the padding around a bubble, or past the first or last
            // line, must still move the caret: the nearest line takes it. Dropping the
            // sample instead made the selection stop at the edge of the text and start
            // moving again only when the pointer came back onto a glyph -- the "drag
            // is stuck" report.
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
        // FOREGROUND, not the chat window's list. The text lives in child windows (one
        // per bubble) and a child renders after its parent, so a highlight on the
        // parent's list is painted first and then covered by the very text it marks --
        // which is why selection looked like it did nothing at all.
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const ImU32 hi = studio_theme::field(&ThemeColors::buttonActive, 0.45f);
        const bool clipped = hasClip_;
        if (clipped) dl->PushClipRect(ImVec2(clipX0_, clipY0_), ImVec2(clipX1_, clipY1_), true);
        for (const auto& vl : frameLines_) {
            int from = 0, to = 0;
            if (!columnSpan(vl.block, vl.line, columnsOf(vl.plain), b0, l0, c0, b1, l1, c1, &from,
                            &to))
                continue;
            // The line's own ruler, the one it was wrapped with. charWidth() here would
            // measure whichever face the caller left pushed.
            const float cw = vl.cw > 0.0f ? vl.cw : 8.0f;
            dl->AddRectFilled(ImVec2(vl.x0 + static_cast<float>(from) * cw, vl.y0),
                              ImVec2(vl.x0 + static_cast<float>(to) * cw, vl.y1), hi);
        }
        if (clipped) dl->PopClipRect();
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
            int from = 0, to = 0;
            if (!columnSpan(vl.block, vl.line, columnsOf(vl.plain), b0, l0, c0, b1, l1, c1, &from,
                            &to))
                continue;
            if (!out.empty()) out += '\n';
            out += textInLine(vl, from, to);
        }
    }
    return out;
}

} // namespace studio
