// proJV studio -- the main screen (see studio_ui.h).
//
// Every string on this screen comes from view_model::Screen. That is the whole
// point of the exercise: this file owns LAYOUT, not wording. If a label, a colour
// reference or a derived summary is wrong here, it is wrong in the TUI too,
// because both read the same derivation.
//
// What changed in the rebuild: the bands are computed by design/layout before
// anything is drawn (see Frame), every control comes from studio/ui (one radius,
// one padding, one hover state), and every string is fitted to its box. There is
// no `static float height` left in this file, and no width counted in characters.
#include "studio_ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "imgui.h"

#include "core/diff_rows.h"
#include "core/speaker_slots.h"
#include "design/actions.h"
#include "design/fit.h"
#include "design/layout.h"
#include "design/tokens.h"
#include "studio_app.h"
#include "studio_input.h"
#include "studio_leftbar.h"
#include "studio_markdown.h"
#include "studio_text.h"
#include "studio_theme.h"
#include "tui/theme_manager.h"
#include "ui/ui.h"

namespace studio {
namespace {

using layout::Rect;
using tokens::Metrics;
using tokens::Radius;
using tokens::Space;
using tokens::Type;
using ui::Palette;


// One text surface for the whole chat: it owns the selection and the layout cache.
TextSurface g_surface;
// The frame of the last drawFrame call: the composer, the slash menu and the
// modals align to the grid the shell computed, not to their own idea of it.
Frame g_frame;

// The TUI's spinner is FTXUI charset 8 (U+2596..), which neither Consolas nor
// Microsoft YaHei carries -- it rendered as a "?" box. A GUI has to use glyphs the
// loaded fonts actually have, so the rotation is the classic ASCII one.
// A drawn arc, not a terminal glyph. "|/-\\" is what a TUI has to work with; in a
// window it reads as a stray character sitting next to a label, and at Caption
// size it is three pixels of ink. One turn per 72 frames, and the phase comes from
// the frame count (not the wall clock), so a screenshot run stays reproducible.
void drawSpinner(const Metrics& m, const layout::Rect& box, ImU32 colour) {
    if (box.w < 6.0f || box.h < 6.0f) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = std::max(2.0f, std::min(box.w, box.h) * 0.26f);
    const ImVec2 c{box.x + box.w * 0.5f, box.y + box.h * 0.5f};
    const float turn = 6.2831853f * static_cast<float>(ImGui::GetFrameCount() % 72) / 72.0f;
    dl->PathClear();
    dl->PathArcTo(c, r, turn, turn + 4.4f, 18); // ~70% of a circle, the gap is the motion
    dl->PathStroke(colour, ImDrawFlags_None, std::max(1.0f, m.hairline * 2.0f));
}

ImU32 fld(const std::string& name) {
    return studio_theme::fieldNamed(name);
}

// A chat row's card colour, straight from the theme's bubble fields: the same
// values the TUI paints its blocks with.
ImU32 bubbleBg(const view_model::ChatRow& row) {
    const ThemeColors& T = ThemeManager::instance().current();
    if (row.kind == "user") return studio_theme::fromHex(T.bubbleUserBg);
    if (row.kind == "assistant" || row.kind == "speaker" || row.kind == "thinking")
        return studio_theme::fromHex(T.bubbleAssistantBg);
    if (row.kind == "system") return studio_theme::fromHex(T.bubbleSystemBg);
    if (row.kind == "tool" || row.kind == "result") return studio_theme::fromHex(T.toolBg);
    return studio_theme::fromHex(T.bubbleDefaultBg);
}

// The view model carries the TUI's terminal labels ("-- You --") so a snapshot is
// byte-identical on both sides. In a window those box-drawing characters read as a
// terminal artefact left on screen, so the studio strips the dashes and renders the
// same words as a small caption in the row's accent colour. Data unchanged,
// presentation de-terminalised.
std::string displayLabel(const std::string& label) {
    static const std::string dash = "\xe2\x94\x80\xe2\x94\x80"; // U+2500 twice
    std::string s = label;
    if (s.rfind(dash + " ", 0) == 0) s = s.substr(dash.size() + 1);
    const std::string tail = " " + dash;
    if (s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0)
        s = s.substr(0, s.size() - tail.size());
    return s;
}

// Speaker/role accent for a row label. The view model carries the TUI's colour
// reference, which for most rows is the plain text colour -- a terminal has no room
// for a second accent. In a window, "who is talking" is exactly what an accent is
// for, so the studio picks a theme field per row kind. Presentation only.
ImU32 labelAccent(const view_model::ChatRow& row) {
    if (row.kind == "user") return studio_theme::field(&ThemeColors::phaseStreaming);
    if (row.kind == "assistant") return studio_theme::field(&ThemeColors::phaseExecutingTools);
    if (row.kind == "system") return studio_theme::field(&ThemeColors::todoTitle);
    if (row.kind == "tool" || row.kind == "result")
        return studio_theme::field(&ThemeColors::toolTitleColor);
    return fld(row.labelColor);
}

} // namespace
} // namespace studio

namespace studio {
namespace {

// ---- diff ----------------------------------------------------------------

void drawDiffUnified(const std::string& diff, float width) {
    const ThemeColors& T = ThemeManager::instance().current();
    const ImU32 add = studio_theme::fromHex(T.todoDone);
    const ImU32 del = studio_theme::fromHex(T.statusError);
    const ImU32 hunk = studio_theme::field(&ThemeColors::textDisabled);
    const ImU32 plain = studio_theme::field(&ThemeColors::text);
    const auto lines = diff_rows::splitLines(diff);
    const int keep = std::min<int>(40, static_cast<int>(lines.size()));
    for (int i = 0; i < keep; ++i) {
        const std::string& line = lines[static_cast<size_t>(i)];
        ImU32 col = plain;
        if (line.rfind("--- ", 0) == 0 || line.rfind("+++ ", 0) == 0) {
            col = hunk;
        } else if (!line.empty()) {
            if (line[0] == '@') col = hunk;
            else if (line[0] == '+') col = add;
            else if (line[0] == '-') col = del;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::TextUnformatted(line.c_str());
        ImGui::PopStyleColor();
    }
    if (static_cast<int>(lines.size()) > keep) {
        studio_md::drawHint("... (" + std::to_string(lines.size() - keep) +
                                " more lines, full diff in session db)",
                            width);
    }
}

// Column helpers for the side-by-side diff: the TUI's cell arithmetic, applied to
// the monospace face the chat is drawn in.
std::string fitCols(const std::string& s, int cols) {
    std::string out;
    int used = 0;
    size_t i = 0;
    while (i < s.size()) {
        const size_t len = fit::codePointLen(s, i);
        const int w = fit::isWide(fit::decode(s, i)) ? 2 : 1;
        if (used + w > cols) break;
        out.append(s, i, len);
        used += w;
        i += len;
    }
    while (used < cols) {
        out += ' ';
        ++used;
    }
    return out;
}

void drawDiffSplit(const std::string& diff, int widthChars, float charW) {
    int total = 0;
    const auto rows = diff_rows::alignRows(diff, diff_rows::kContextLines, 40, &total);
    int maxNumber = 0;
    for (const auto& r : rows) maxNumber = std::max(maxNumber, std::max(r.oldNo, r.newNo));
    const int gutter = std::max(3, diff_rows::digitCount(maxNumber));
    const int pane = (widthChars - 2 * gutter - 5) / 2;
    if (pane < 12) {
        drawDiffUnified(diff, static_cast<float>(widthChars) * charW);
        return;
    }
    const ThemeColors& T = ThemeManager::instance().current();
    const ImU32 add = studio_theme::fromHex(T.todoDone);
    const ImU32 del = studio_theme::fromHex(T.statusError);
    const ImU32 gut = studio_theme::field(&ThemeColors::textDisabled);
    const ImU32 plain = studio_theme::field(&ThemeColors::text);

    const auto gutterCell = [gutter](int number) {
        std::string s = number > 0 ? std::to_string(number) : std::string();
        while (static_cast<int>(s.size()) < gutter) s.insert(s.begin(), ' ');
        return s;
    };
    const auto col = [](const std::string& s, ImU32 c) {
        ImGui::PushStyleColor(ImGuiCol_Text, c);
        ImGui::TextUnformatted(s.c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine(0.0f, 0.0f);
    };

    for (const auto& r : rows) {
        if (r.isHeader) {
            col(fitCols(r.headerText, widthChars), gut);
            ImGui::NewLine();
            continue;
        }
        // The marker stays inside the pane so a colour-blind theme still shows
        // which side changed.
        const std::string left = (r.oldIsChange ? "-" : " ") + r.oldText;
        const std::string right = (r.newIsChange ? "+" : " ") + r.newText;
        col(gutterCell(r.oldNo), gut);
        col(" ", plain);
        col(fitCols(left, pane), r.oldIsChange ? del : plain);
        col(" \xe2\x94\x82 ", gut);
        col(gutterCell(r.newNo), gut);
        col(" ", plain);
        col(fitCols(right, pane), r.newIsChange ? add : plain);
        ImGui::NewLine();
    }
    if (total > static_cast<int>(rows.size())) {
        studio_md::drawHint("... (" + std::to_string(total - static_cast<int>(rows.size())) +
                                " more rows, full diff in session db)",
                            static_cast<float>(widthChars) * charW);
    }
}

void drawDiff(const std::string& diff, float width) {
    const float cw = std::max(1.0f, ImGui::CalcTextSize("M").x);
    const int widthChars = static_cast<int>(width / cw);
    if (widthChars >= diff_rows::kSplitMinWidth)
        drawDiffSplit(diff, widthChars, cw);
    else
        drawDiffUnified(diff, width);
}

} // namespace
} // namespace studio

namespace studio {
namespace {

// ---- chat ----------------------------------------------------------------

// The body of one row, drawn inside the current window at the current cursor.
// `blockId` keys the cached text layout (see studio_text.h); it is stable per row
// and per part, so a settled row and the streaming block never share a cache slot.
void drawRowBody(const Metrics& m, const view_model::ChatRow& row, int blockId, bool streaming) {
    (void)streaming;
    // A right margin inside the card, not just the child's padding. Two steps: the
    // wrap is computed from measured token widths, and a CJK glyph or a rounding in
    // the measurer can put a line a few pixels past the box it was fitted to. With a
    // margin that size the last word breaks to the next line instead of being cut,
    // and a paragraph reads as set rather than crammed against its border.
    const float textW = std::max(40.0f, ImGui::GetContentRegionAvail().x -
                                             2.0f * tokens::spacePx(m, Space::Lg));
    if (!row.reasoningSummary.empty()) ui::hint(m, row.reasoningSummary);
    if (!row.reasoningHead.empty() && !row.reasoningBody.empty())
        ui::text(m, row.reasoningHead, fld(row.labelColor), Type::Small);
    if (row.earlierOmitted) ui::hint(m, "... (earlier thinking omitted)");
    if (!row.reasoningBody.empty())
        g_surface.drawBlock(blockId + 1, row.reasoningBody, false, textW, fld(row.textColor));
    if (!row.text.empty()) {
        if (row.kind == "diff") {
            drawDiff(row.text, textW);
        } else {
            // A markdown body must NOT get a plain-colour override: the override
            // applies to every run, which flattens headings, code, links and
            // tables into one colour. Only genuinely plain text passes a colour.
            g_surface.drawBlock(blockId + 2, row.text, row.markdown, textW,
                                row.markdown ? 0 : fld(row.textColor));
        }
    }
    // An expanded tool row shows the results UNCAPPED (`extraFull`); `extra` is
    // the five-line cap the TUI draws, kept so the two front-ends stay comparable.
    const std::string& results = row.extraFull.empty() ? row.extra : row.extraFull;
    if (!results.empty())
        g_surface.drawBlock(blockId + 3, results, false, textW, fld(row.extraColor));
    // The diff belongs to the call that produced it, so it is drawn inside the
    // same card, under its results -- not as a card of its own.
    if (!row.diffText.empty()) {
        ImGui::Dummy(ImVec2(1.0f, tokens::spacePx(m, Space::Xs)));
        drawDiff(row.diffText, textW);
    }
}

// Truncate `s` to `room` pixels, measured with the font that is about to draw it.
// ui::fitLine() takes a tier instead, and in the chat the tier in use is Mono: a
// measure/fit pair at a different tier truncates the wrong amount ("read_fi..."
// for a string that had room for thirty characters).
std::string clipToWidth(const std::string& s, float room) {
    if (room <= 0.0f) return std::string();
    if (ImGui::CalcTextSize(s.c_str()).x <= room) return s;
    const float dots = ImGui::CalcTextSize("...").x;
    size_t lo = 0, hi = s.size();
    while (lo < hi) {
        const size_t mid = (lo + hi + 1) / 2;
        size_t cut = mid < s.size() ? mid : s.size();
        while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
        if (ImGui::CalcTextSize(s.substr(0, cut).c_str()).x + dots <= room)
            lo = mid;
        else
            hi = mid - 1;
    }
    size_t cut = lo;
    while (cut > 0 && cut < s.size() && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return s.substr(0, cut) + "...";
}

// Outcome colour: the same three words a status line would use.
ImU32 statusColour(const std::string& status) {
    if (status == "failed") return studio_theme::field(&ThemeColors::statusError);
    if (status == "pending") return studio_theme::field(&ThemeColors::textDisabled);
    return studio_theme::field(&ThemeColors::todoDone);
}

// A folded tool block: the disclosure triangle, the first call of the message, the
// outcome and the size of the change -- one line where the block used to be a card
// with five lines of result text in it. Calls 2..N of the same message stay
// visible under it: the call list is the audit trail, the results are the noise.
void drawFoldedTool(const Metrics& m, const view_model::ChatRow& row) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float lh = ImGui::GetTextLineHeight();
    const float rowW = ImGui::GetContentRegionAvail().x;
    const float arrow = lh * 0.34f;
    const float cx = p.x + lh * 0.38f;
    const float cy = p.y + lh * 0.5f;
    dl->AddTriangleFilled(ImVec2(cx - arrow * 0.55f, cy - arrow), ImVec2(cx + arrow, cy),
                          ImVec2(cx - arrow * 0.55f, cy + arrow),
                          studio_theme::field(&ThemeColors::toolTitleColor));
    // Right edge, in order: the outcome, the call's own clock, then the size of
    // what is folded away (a diff's line counts when the call changed a file, the
    // result's length otherwise). Each part keeps its own colour and the whole
    // group is laid out from the right, so the left text can be fitted to the room
    // that is actually left -- "ok 12.4s 43 lines" must not sit on the call name.
    // The diff size only. The result's line count was a second number competing
    // with the call name for the same pixels, and it says nothing a reader cannot
    // see by opening the row.
    const std::string& size = row.diffSummary;
    // Two blocks on the right: the outcome in its own colour, then the clock and
    // the size as one dim run. Each is measured by the same call that draws it, so
    // the group cannot claim more room than it uses (it used to, and the call name
    // was cut to eight characters to make room for nothing).
    std::string tail = row.durationText;
    if (!size.empty()) tail += (tail.empty() ? "" : "  ") + size;
    const float pad = lh * 0.5f;
    const float statusW = row.status.empty() ? 0.0f : ImGui::CalcTextSize(row.status.c_str()).x;
    const float tailW = tail.empty() ? 0.0f : ImGui::CalcTextSize(tail.c_str()).x;
    const float right = statusW + tailW + ((statusW > 0.0f && tailW > 0.0f) ? pad : 0.0f);
    // A full step of air between the call name and the right-hand figures: the
    // clipped name already ends in "...", and an ellipsis that touches "ok" reads
    // as one long word.
    const float leftRoom = std::max(lh * 2.0f, rowW - lh * 1.1f - right - lh * 1.1f);
    ImGui::Dummy(ImVec2(lh * 1.1f, lh));
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, studio_theme::field(&ThemeColors::toolTitleColor));
    ImGui::TextUnformatted(clipToWidth(row.summary, leftRoom).c_str());
    ImGui::PopStyleColor();

    float x = std::max(lh * 1.1f, rowW - right);
    if (statusW > 0.0f) {
        ImGui::SameLine(x);
        ImGui::PushStyleColor(ImGuiCol_Text, statusColour(row.status));
        ImGui::TextUnformatted(row.status.c_str());
        ImGui::PopStyleColor();
        x += statusW + pad;
    }
    if (tailW > 0.0f) {
        ImGui::SameLine(x);
        ImGui::PushStyleColor(ImGuiCol_Text, studio_theme::field(&ThemeColors::toolResultText));
        ImGui::TextUnformatted(tail.c_str());
        ImGui::PopStyleColor();
    }

    // The rest of the call list, one indented line each.
    const float indent = lh * 1.1f;
    size_t nl = row.text.find('\n');
    while (nl != std::string::npos) {
        const size_t end = row.text.find('\n', nl + 1);
        const std::string line = row.text.substr(
            nl + 1, (end == std::string::npos ? row.text.size() : end) - (nl + 1));
        if (!line.empty()) {
            ImGui::Dummy(ImVec2(1.0f, tokens::spacePx(m, Space::Xs)));
            ImGui::Indent(indent);
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  studio_theme::field(&ThemeColors::toolResultText));
            ImGui::TextUnformatted(clipToWidth(line, rowW - indent).c_str());
            ImGui::PopStyleColor();
            ImGui::Unindent(indent);
        }
        nl = end;
    }
}

// Returns true when the row consumed the click this frame: the text-selection
// surface listens to the same press and must stay out of it.
bool drawChatRow(const Frame& f, const view_model::ChatRow& row, float width, int index,
                 App& app, int& blockSeq) {
    const Metrics& m = f.metrics;
    const float padX = tokens::spacePx(m, Space::Md);
    // Tighter than the block padding: the transcript is a list of cards, and a card
    // that pads like a dialog makes ten tool rows read as ten dialogs.
    const float padY = tokens::spacePx(m, Space::Xs);
    // A row that carries a label is a message: it gets a card. A bare text row is
    // a continuation of the one above it and stays on the surface.
    const bool carded = !row.label.empty() || row.kind == "tool" || row.kind == "result" ||
                        row.kind == "diff" || row.kind == "system" ||
                        row.kind == "skillrun";
    // A frame-local counter, not the row index: a container's children need ids
    // that cannot collide with the next row's, and index*10 ran out of room the
    // moment a block could hold children of its own.
    const int blockId = blockSeq++ * 10;
    bool clicked = false;
    ImGui::PushID(index);
    if (carded) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, bubbleBg(row));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, tokens::radiusPx(m, Radius::Md));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padX, padY));
        const ImGuiChildFlags flags =
            ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding;
        // NoScrollWithMouse (and NOT NoScrollbar, which would cancel it) hands the
        // wheel back to the chat area. Without it every bubble was its own little
        // window that swallowed the wheel: the transcript only scrolled when the
        // pointer happened to sit in a gap between cards. That is the "scrolling does
        // not work" report.
        if (ImGui::BeginChild("##bubble", ImVec2(width, 0.0f), flags,
                              ImGuiWindowFlags_NoScrollWithMouse)) {
            if (row.kind == "skillrun") {
                // A skill run's role block. The header stays in BOTH states: it is
                // the fold's own hit target, and a header that disappears when the
                // block opens leaves no way back.
                drawFoldedTool(m, row);
                if (!row.collapsed) {
                    const float indent = ImGui::GetTextLineHeight() * 1.1f;
                    for (const auto& child : row.children) {
                        ImGui::Dummy(ImVec2(1.0f, tokens::spacePx(m, Space::Xs)));
                        ImGui::Indent(indent);
                        if (!child.label.empty())
                            ui::text(m, displayLabel(child.label), labelAccent(child),
                                     Type::Caption);
                        drawRowBody(m, child, blockSeq++ * 10, false);
                        ImGui::Unindent(indent);
                    }
                }
            } else if (row.collapsed) {
                drawFoldedTool(m, row);
            } else {
                if (!row.label.empty()) {
                    ui::text(m, displayLabel(row.label), labelAccent(row), Type::Caption);
                    // Outcome, clock and change size ride on the label line: a
                    // settled tool block says whether it worked without being opened.
                    std::string right = row.status;
                    for (const std::string* part : {&row.durationText, &row.diffSummary}) {
                        if (part->empty()) continue;
                        right += (right.empty() ? "" : "  ") + *part;
                    }
                    if (!right.empty()) {
                        ImGui::SameLine();
                        ui::text(m, right,
                                 row.status.empty() ? fld(row.extraColor) : statusColour(row.status),
                                 Type::Caption);
                    }
                    ImGui::Dummy(ImVec2(1.0f, tokens::spacePx(m, Space::Xs)));
                }
                drawRowBody(m, row, blockId, false);
            }
        }
        ImGui::EndChild();
        // The hit box is taken AFTER EndChild, which makes the child the last
        // item: its rect is known here, on the same frame the mouse position is.
        // An InvisibleButton inside the card would be an item in the flow -- with
        // AutoResizeY it would change the height the child just computed, and it
        // would have to be kept out of the wheel again.
        if (row.collapsible) {
            const ImVec2 p0 = ImGui::GetItemRectMin();
            const ImVec2 p1 = ImGui::GetItemRectMax();
            const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
                                 ImGui::IsMouseHoveringRect(p0, p1, false);
            if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (hovered && !row.key.empty() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                app.toggleRowCollapsed(row.key);
                clicked = true;
            }
        }
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    } else {
        drawRowBody(m, row, blockId, false);
    }
    ImGui::PopID();
    // Half a step between cards: enough that two blocks never touch, little
    // enough that the transcript stops reading as a stack of separate windows.
    ImGui::Dummy(ImVec2(1.0f, tokens::spacePx(m, Space::Xs) * 0.5f));
    return clicked;
}

void drawStreaming(const Frame& f, const view_model::StreamingView& st, float width) {
    if (!st.visible) return;
    const Metrics& m = f.metrics;
    const Palette p = ui::palette();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, studio_theme::field(&ThemeColors::bubbleAssistantBg));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, tokens::radiusPx(m, Radius::Md));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(tokens::spacePx(m, Space::Md), tokens::spacePx(m, Space::Sm)));
    const ImGuiChildFlags flags =
        ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding;
    if (ImGui::BeginChild("##streaming", ImVec2(width, 0.0f), flags,
                          ImGuiWindowFlags_NoScrollWithMouse)) {
        const float textW =
            std::max(40.0f, ImGui::GetContentRegionAvail().x - 2.0f * tokens::spacePx(m, Space::Lg));
        if (st.hasReasoning) {
            if (!st.reasoningHead.empty())
                ui::text(m, st.reasoningHead, studio_theme::field(&ThemeColors::reasoningTextColor),
                         Type::Small);
            if (st.earlierOmitted) ui::hint(m, "... (earlier thinking omitted)");
            g_surface.drawBlock(-11, st.reasoningBody, false, textW,
                                studio_theme::field(&ThemeColors::reasoningBodyText));
        }
        if (st.hasContent) {
            if (!st.contentHead.empty())
                ui::text(m, st.contentHead,
                         studio_theme::field(&ThemeColors::phaseExecutingTools), Type::Caption);
            // Same renderer as a settled row: the streaming and persisted paths used
            // to be two different renderers, so a table that showed while streaming
            // disappeared when the message settled.
            g_surface.drawBlock(-12, st.content, true, textW,
                                studio_theme::field(&ThemeColors::text));
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(1.0f, tokens::spacePx(m, Space::Xs)));
}

void drawChat(App& app, const Frame& f) {
    const Metrics& m = f.metrics;
    const Palette p = ui::palette();
    const view_model::Screen& s = app.screen();

    ui::card(f.chat, m, false);
    const layout::Rect inner = f.chat.inset(tokens::spacePx(m, Space::Sm));
    const bool open = ui::beginScroll("##chat", inner, m, p.surface);
    const int req = app.takeScrollRequest();
    if (open) {
        if (req != 0) ui::scrollBy(-static_cast<float>(req) * tokens::linePx(m, Type::Body));
        const float avail = ImGui::GetContentRegionAvail().x;
        const float colW = std::min(avail, m.readingMax);
        // A centred reading column: full-bleed text across a 4K window is
        // unreadable, and that is what made the old window look like a stretched
        // terminal.
        const float inset = std::max(0.0f, (avail - colW) * 0.5f);
        if (inset > 0.0f) ImGui::Indent(inset);
        // Monospace inside the reading column: code, tables and box drawing must
        // align, and a terminal chat log is the one place a mono face is right.
        ui::pushType(m, Type::Mono, true);
        bool rowClick = false;
        int blockSeq = 0;
        for (int i = 0; i < static_cast<int>(s.chat.size()); ++i)
            rowClick |= drawChatRow(f, s.chat[static_cast<size_t>(i)], colW, i, app, blockSeq);
        drawStreaming(f, s.streaming, colW);
        ui::popType();
        if (s.chat.empty() && !s.streaming.visible) {
            // Centred in the visible area rather than glued to the top: an empty
            // window with one line of grey text at its top edge reads as a
            // rendering failure, not as an invitation.
            const float half = std::max(0.0f, ImGui::GetWindowHeight() * 0.38f);
            ImGui::Dummy(ImVec2(1.0f, half));
            ImGui::PushStyleColor(ImGuiCol_Text, p.textDim);
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + colW);
            ui::pushType(m, Type::Body, false);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (colW - 320.0f * m.factor) * 0.5f));
            ImGui::TextUnformatted(
                "(no messages yet -- type below, Enter sends, / opens the command list)");
            ui::popType();
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
        // Mouse selection and Ctrl+C over everything drawn above -- but NOT on the
        // frame a tool block was opened or closed: the selection hit-tests raw
        // mouse clicks (studio_text.cpp), so without this one press would both
        // toggle the row and start selecting a line inside it.
        if (!rowClick) g_surface.handleInput();
        if (inset > 0.0f) ImGui::Unindent(inset);
    }
    // Sticky bottom, but only for a reader who is already near it. The old rule
    // was a binary "was at the bottom last frame": one wheel notch stopped the
    // chase for good, and an arriving message re-armed it and tore the view down
    // from wherever it was. Now:
    //   - an explicit jump (send, new chat, a notice) pins once, whatever the
    //     distance -- the user just did something and wants to see it;
    //   - otherwise the view follows only while it is within a third of a screen
    //     of the bottom, which is far enough that a streaming frame cannot lose
    //     the chase and near enough that scrolling away really is "leave me be".
    const bool jump = open && app.takeFollowBottom();
    const float slack = std::max(4.0f * tokens::linePx(m, Type::Body),
                                 ImGui::GetWindowHeight() * 0.33f);
    const bool follow = jump || (open && ui::scrollFromBottom() <= slack);
    ui::endScroll(follow);
}

// ---- dock ----------------------------------------------------------------

void drawDock(App& app, const Frame& f) {
    const Metrics& m = f.metrics;
    const Palette p = ui::palette();
    const view_model::Screen& s = app.screen();
    ui::card(f.dock, m, false);
    const layout::Rect inner =
        f.dock.inset(tokens::spacePx(m, Space::Md), tokens::spacePx(m, Space::Md),
                     tokens::spacePx(m, Space::Md), tokens::spacePx(m, Space::Md));
    const bool open = ui::beginScroll("##dock", inner, m, p.surface);
    if (open) {
        ui::text(m, s.todoTitle, studio_theme::field(&ThemeColors::todoTitle), Type::Heading);
        ImGui::Dummy(ImVec2(1.0f, tokens::spacePx(m, Space::Xs)));
        if (s.todoEmpty) {
            ui::hint(m, "(No active tasks)");
        } else {
            for (const auto& row : s.todo) {
                const std::string ThemeColors::*color = &ThemeColors::todoPending;
                if (row.kind == "done")
                    color = &ThemeColors::todoDone;
                else if (row.kind == "inprogress")
                    color = &ThemeColors::todoInProgress;
                else if (row.kind == "open")
                    color = &ThemeColors::todoOpen;
                const ImU32 col = studio_theme::field(color);
                // Bullet, then the text with a hanging indent: a wrapped task line
                // stays aligned under its first character.
                const float bullet = tokens::spacePx(m, Space::Md);
                const ImVec2 at = ImGui::GetCursorScreenPos();
                const float r = std::max(1.5f, m.hairline * 1.5f);
                ImGui::GetWindowDrawList()->AddCircleFilled(
                    ImVec2(at.x + r, at.y + tokens::linePx(m, Type::Small) * 0.45f), r, col);
                ImGui::Indent(bullet);
                studio_md::drawPlain(row.text, ImGui::GetContentRegionAvail().x, col);
                ImGui::Unindent(bullet);
            }
        }
        if (!s.tasksEmpty) {
            ui::divider(m);
            for (const auto& t : s.tasks) {
                const std::string line = t.tag + "  " + t.detail;
                ui::text(m, ui::fitLine(m, line, ImGui::GetContentRegionAvail().x, Type::Small,
                                        false),
                         p.textDim, Type::Small);
            }
        }
    }
    ui::endScroll(false);
}

} // namespace
} // namespace studio

namespace studio {
namespace {

// ---- header --------------------------------------------------------------

// One agent unit, placed: "[Penny] * narration". A fixed cell keeps an idle agent
// from shifting its neighbours, exactly like the TUI's size(WIDTH, EQUAL, cols).
void drawAgentUnit(const Metrics& m, const layout::Rect& box, const view_model::AgentRow& a) {
    const Palette p = ui::palette();
    const float pad = tokens::spacePx(m, Space::Xs);
    layout::Rect rest = box;
    const float tagW = std::min(rest.w, ui::measure(m, a.tag, Type::Caption, false) + pad);
    ui::textAt(m, layout::Rect{rest.x, rest.y, tagW, rest.h},
               a.idle ? a.tag : a.tag, a.idle ? p.textDim : p.text, Type::Caption, false, 0.0f);
    rest.x += tagW;
    rest.w -= tagW;
    if (!a.idle && rest.w > m.iconSize) {
        const ImU32 glyph = a.activity == "error"
                                ? p.danger
                                : studio_theme::fromHex(
                                      ThemeManager::instance().current().*
                                      speaker_slots::paletteField(static_cast<size_t>(a.slot)));
        drawSpinner(m, layout::Rect{rest.x, rest.y, m.iconSize + pad, rest.h}, glyph);
        rest.x += m.iconSize + pad;
        rest.w -= m.iconSize + pad;
    }
    if (rest.w > 0.0f) {
        ui::textAt(m, rest,
                   ui::fitLine(m, a.detail, rest.w, Type::Caption, false), p.textDim, Type::Caption,
                   false, 0.0f);
    }
}

// ---- header actions ------------------------------------------------------

// The header's buttons, planned ONCE per frame. The same widths size the band
// (computeFrame) and draw the buttons (drawHeader), so a wrap can never disagree
// with the space that was reserved for it -- the class of bug that made every
// previous "the layout is a frame behind" report.
struct HeaderPlan {
    std::vector<std::string> labels;
    std::vector<std::string> tooltips;
    std::vector<float> widths;
    std::vector<std::vector<int>> rows;
    float gap = 0.0f;
    float rowH = 0.0f;
    float height = 0.0f;
    float firstRowW = 0.0f;
};

HeaderPlan g_headerPlan;

GKey keyFor(actions::Id id) {
    switch (id) {
    case actions::Id::Role:
        return GKey::Tab;
    case actions::Id::NewChat:
        return GKey::F3;
    case actions::Id::Sessions:
        return GKey::F4;
    case actions::Id::Model:
        return GKey::F10;
    case actions::Id::Reasoning:
        return GKey::F8;
    case actions::Id::Dock:
        return GKey::F9;
    case actions::Id::Theme:
        return GKey::F6;
    case actions::Id::ThemeEditor:
        return GKey::F7;
    case actions::Id::Mcp:
        return GKey::F5;
    case actions::Id::Config:
        return GKey::F2;
    case actions::Id::Help:
        return GKey::F1;
    }
    return GKey::None;
}

// The header's buttons are Ghost: the strip and a Secondary button share one
// surface colour, so eleven filled buttons read as eleven floating words. Hover
// is where the fill and the shortcut appear anyway.
const ui::Btn kHeaderButton = ui::Btn::Ghost;

actions::State headerState(const App& app) {
    const view_model::Screen& s = app.screen();
    actions::State st;
    st.dock = s.dock;
    st.reasoning = app.source().reasoningExpanded;
    st.role = s.roleName;
    st.promptFiles = s.promptFiles;
    return st;
}

float rowWidth(const std::vector<float>& widths, const std::vector<int>& row, float gap) {
    float w = 0.0f;
    for (size_t i = 0; i < row.size(); ++i) {
        w += widths[static_cast<size_t>(row[i])] + (i == 0 ? 0.0f : gap);
    }
    return w;
}

HeaderPlan planHeaderActions(const App& app, const Metrics& m, float rowW) {
    HeaderPlan plan;
    const std::vector<actions::Item>& items = actions::header();
    const actions::State st = headerState(app);
    plan.gap = tokens::spacePx(m, Space::Sm);
    plan.rowH = m.controlH;
    for (const actions::Item& it : items) {
        plan.labels.push_back(actions::label(it.id, st));
        plan.tooltips.push_back(actions::tooltip(it.id, st));
    }
    for (size_t i = 0; i < plan.labels.size() && i < items.size(); ++i) {
        const float w = ui::buttonWidth(m, plan.labels[i], kHeaderButton);
        plan.widths.push_back(std::min(std::max(1.0f, rowW), w));
    }
    // The wordmark keeps its room; the pills and the spinner give way first, and
    // the buttons never do. Row 0 fits next to the title, later rows get the width.
    const float reserve =
        ui::measure(m, "proJV", Type::Title, false) + tokens::spacePx(m, Space::Lg);
    plan.rows = actions::pack(plan.widths, plan.gap, std::max(0.0f, rowW - reserve), rowW);
    if (!plan.rows.empty()) plan.firstRowW = rowWidth(plan.widths, plan.rows[0], plan.gap);
    const float n = static_cast<float>(plan.rows.size());
    plan.height = n > 0.0f ? n * plan.rowH + (n - 1.0f) * plan.gap : 0.0f;
    return plan;
}

void drawHeader(App& app, const Frame& f) {
    const Metrics& m = f.metrics;
    const Palette p = ui::palette();
    const view_model::Screen& s = app.screen();
    ui::fill(f.header, p.surfaceAlt);
    // A hairline under the header is what separates the chrome from the content;
    // a colour change alone was not enough at 150% DPI.
    ui::fill(layout::Rect{f.header.x, f.header.bottom() - m.hairline, f.header.w, m.hairline},
             p.border);

    const float pad = tokens::spacePx(m, Space::Lg);
    const layout::Rect row = f.header.inset(pad, tokens::spacePx(m, Space::Sm), pad,
                                            tokens::spacePx(m, Space::Sm));
    const float titleH = tokens::linePx(m, Type::Title);
    const float row0H = std::max(titleH, m.controlH);
    const HeaderPlan& plan = g_headerPlan;
    const std::vector<actions::Item>& items = actions::header();

    // Every key the product documents is a button (design/actions.h), right
    // aligned, wrapping into another row when the window is narrow. Nothing is
    // dropped: a hidden action is a function the user cannot find.
    for (size_t r = 0; r < plan.rows.size(); ++r) {
        const std::vector<int>& idxs = plan.rows[r];
        float x = row.right() - rowWidth(plan.widths, idxs, plan.gap);
        const float y = row.y + (row0H - plan.rowH) * 0.5f +
                        static_cast<float>(r) * (plan.rowH + plan.gap);
        for (int idx : idxs) {
            if (idx < 0 || idx >= static_cast<int>(items.size())) continue;
            const size_t i = static_cast<size_t>(idx);
            const actions::Item& item = items[i];
            const layout::Rect box{x, y, plan.widths[i], plan.rowH};
            const std::string shown =
                ui::fitLine(m, plan.labels[i], plan.widths[i] - 2.0f * tokens::spacePx(m, Space::Md),
                            Type::Body, false);
            if (ui::buttonAt(m, box, shown, kHeaderButton, plan.tooltips[i].c_str())) {
                app.handleKey(keyFor(item.id));
            }
            x += plan.widths[i] + plan.gap;
        }
    }
    const float leftW =
        std::max(0.0f, row.w - (plan.firstRowW > 0.0f ? plan.firstRowW + plan.gap : 0.0f));

    // Title: the largest tier that fits the space that is left, MEASURED so the
    // pills can follow the text instead of floating at the right edge of a box
    // that is mostly empty.
    const std::string titleText = "proJV";
    const std::string tag = "studio";
    const std::string phase = s.phaseName;
    const float tagW =
        ui::measure(m, tag, Type::Caption, false) + 2.0f * tokens::spacePx(m, Space::Sm);
    const float phaseW =
        ui::measure(m, phase, Type::Caption, false) + 2.0f * tokens::spacePx(m, Space::Sm);
    const float spinnerW = s.busy ? m.iconSize + tokens::spacePx(m, Space::Xs) : 0.0f;
    const float pillsW = tagW + tokens::spacePx(m, Space::Lg) + phaseW + spinnerW;
    const float minTitleW = 72.0f * m.factor;

    Type titleTier = Type::Display;
    float titleW = ui::measure(m, titleText, titleTier, false);
    const float budget = leftW - pillsW - tokens::spacePx(m, Space::Lg);
    if (titleW > budget) {
        titleTier = Type::Title;
        titleW = ui::measure(m, titleText, titleTier, false);
    }
    if (titleW > budget) {
        titleTier = Type::Heading;
        titleW = ui::measure(m, titleText, titleTier, false);
    }
    titleW = std::min(titleW, std::max(minTitleW, budget));

    const bool showPills = budget >= minTitleW;
    const layout::Rect titleBox{row.x, row.y + (row0H - titleH) * 0.5f, titleW, titleH};
    ui::textAt(m, titleBox, ui::fitLine(m, titleText, titleW, titleTier, false), p.text,
               titleTier, false, 0.0f, 0.5f);
    if (showPills) {
        float x = titleBox.right() + tokens::spacePx(m, Space::Lg);
        const float pillH =
            tokens::linePx(m, Type::Caption) + 2.0f * tokens::spacePx(m, Space::Xs);
        const float pillY = row.y + (row0H - pillH) * 0.5f;
        ui::badge(m, layout::Rect{x, pillY, tagW, pillH}, tag, p.textDim, p.surface);
        x += tagW + tokens::spacePx(m, Space::Lg);
        const ImU32 phaseBg = fld(s.phaseColor);
        ui::badge(m, layout::Rect{x, pillY, phaseW, pillH},
                  ui::fitLine(m, phase, phaseW - tokens::spacePx(m, Space::Sm), Type::Caption,
                              false),
                  ui::contrastOn(phaseBg), phaseBg);
        x += phaseW + tokens::spacePx(m, Space::Xs);
        if (s.busy) {
            drawSpinner(m, layout::Rect{x, pillY, m.iconSize, pillH}, p.accent);
        }
    }

    // Agent strip: packed into whole rows by the frame, drawn here.
    if (!s.agentsEmpty && !f.headerAgentRows.empty()) {
        const float unitGap = tokens::spacePx(m, Space::Xl);
        for (size_t r = 0; r < f.headerAgentRows.size(); ++r) {
            const std::vector<int>& idxs = f.headerAgentRows[r];
            if (idxs.empty()) continue;
            const int first = idxs.front();
            const int last = idxs.back();
            const float unitW = (leftW - unitGap * static_cast<float>(idxs.size() - 1)) /
                                static_cast<float>(std::max<size_t>(1, idxs.size()));
            (void)first;
            (void)last;
            float x = row.x;
            const float y = f.headerAgentRowsY + static_cast<float>(r) * f.headerAgentRowH;
            for (int idx : idxs) {
                const view_model::AgentRow& a = s.agents[static_cast<size_t>(idx)];
                drawAgentUnit(m, layout::Rect{x, y, unitW, f.headerAgentRowH}, a);
                x += unitW + unitGap;
            }
        }
    }
}

// ---- footer --------------------------------------------------------------

void drawStatusLine(App& app, const Frame& f) {
    const Metrics& m = f.metrics;
    const Palette p = ui::palette();
    const view_model::Screen& s = app.screen();
    layout::Rect box = f.statusLine;
    if (box.w < 16.0f) return;
    // The column beside the prompt holds two lines: the state on top, the turn's
    // own figures (or the skill pill) under it. Both are right-aligned to the
    // prompt's right edge, so a long "Running: read_file (2/5)" grows leftwards
    // into free space instead of pushing the box around.
    // One line (the state) unless there is something for the second one to say, in
    // which case "Idle" sits at the top of a two-line stack instead of floating in
    // the middle of a box twice its height.
    const std::string work = app.workSummary();
    const bool hasSecond = !s.skillStatus.empty() || !work.empty();
    const float lineH = hasSecond ? std::max(1.0f, box.h * 0.5f) : box.h;
    const layout::Rect top{box.x, box.y, box.w, lineH};
    const layout::Rect bottom{box.x, box.y + lineH, box.w, box.h - lineH};

    const float spinnerW = s.statusSpinner ? tokens::spacePx(m, Space::Md) : 0.0f;
    // A full step between the arc and the word: at an Xs gap the ring touched the
    // first glyph.
    const float gapX = s.statusSpinner ? tokens::spacePx(m, Space::Sm) : 0.0f;
    float x = top.x;
    if (s.statusSpinner) {
        drawSpinner(m, layout::Rect{x, top.y, spinnerW, top.h}, p.accent);
        x += spinnerW + gapX;
    }
    // Body, with Small only as the fallback for a line too long to fit: the footer
    // is read at a glance, and 13.5 px of grey was reported as too small twice.
    const Type tiers[2] = {Type::Body, Type::Small};
    // Left-aligned inside its column: right-aligning pushed "Idle" to the far edge
    // of the window, a hand's width away from the box it describes.
    // From `x`, i.e. after the spinner: starting at the column's own left edge drew
    // "Idle" underneath the arc.
    ui::fittedText(m, layout::Rect{x, top.y, std::max(1.0f, top.right() - x), top.h},
                   s.statusText, fld(s.statusColor), tiers, 2, false, 0.0f);

    if (!s.skillStatus.empty()) {
        const std::string pill = "[skill] " + s.skillStatus;
        const float w = std::min(bottom.w, ui::measure(m, pill, Type::Caption, false) +
                                            2.0f * tokens::spacePx(m, Space::Sm));
        const float h = std::min(bottom.h, tokens::linePx(m, Type::Caption) +
                                            2.0f * tokens::spacePx(m, Space::Xs));
        const layout::Rect pillBox{bottom.right() - w, bottom.y + (bottom.h - h) * 0.5f, w, h};
        ui::badge(m, pillBox, ui::fitLine(m, pill, pillBox.w - tokens::spacePx(m, Space::Md),
                                          Type::Caption, false),
                  ui::contrastOn(p.warning), p.warning);
        return;
    }
    // How long this has been running and how fast it is producing. Studio clock
    // (the view model reads none), so it is drawn here, not derived into the
    // snapshot.
    if (!work.empty() && hasSecond) {
        const float w = std::min(bottom.w, ui::measure(m, work, Type::Small, false));
        ui::textAt(m, layout::Rect{bottom.x, bottom.y, w, bottom.h},
                   ui::fitLine(m, work, w, Type::Small, false), p.textDim, Type::Small, false, 0.0f,
                   0.5f);
    }
}

void drawQueued(App& app, const Frame& f) {
    if (app.screen().queued <= 0) return;
    const Metrics& m = f.metrics;
    const view_model::Screen& s = app.screen();
    const std::string text =
        "Queued: " + std::to_string(s.queued) + " message(s) - sends after current turn";
    ui::textAt(m, f.queued, text, studio_theme::field(&ThemeColors::todoTitle), Type::Body, false,
               0.0f);
}

void drawStatusBar(App& app, const Frame& f) {
    const Metrics& m = f.metrics;
    const Palette p = ui::palette();
    const view_model::Screen& s = app.screen();
    // The darkest surface in the window: the bar's own colours are mid-tone status
    // colours, and on the lighter alt surface the figures were unreadable.
    ui::fill(f.statusBar, p.inputBg);
    // The service bar is reference data (tokens, context, cost), not a footnote:
    // at Body it reads at arm's length, at Small it read as a caption.
    const Type tier = Type::Body;
    const float gap = tokens::spacePx(m, Space::Sm);
    float x = f.statusBar.x + tokens::spacePx(m, Space::Md);
    const float limit = f.statusBar.right() - tokens::spacePx(m, Space::Md);
    for (size_t i = 0; i < s.statusBar.size(); ++i) {
        const view_model::Segment& seg = s.statusBar[i];
        const float w = ui::measure(m, seg.text, tier, false);
        if (x + w > limit) break;
        if (i > 0) {
            const float sepW = ui::measure(m, "  |  ", tier, false);
            if (x + sepW + w > limit) break;
            ui::textAt(m, layout::Rect{x, f.statusBar.y, sepW, f.statusBar.h}, "  |  ", p.divider,
                       tier, false, 0.0f);
            x += sepW;
        }
        ui::textAt(m, layout::Rect{x, f.statusBar.y, w, f.statusBar.h}, seg.text, fld(seg.color),
                   tier, false, 0.0f);
        x += w + gap * 0.5f;
    }
    // The receipt: what the last click did, at the right end of the same bar. It
    // expires on its own, so a stale claim never sits there looking current.
    const std::string& receipt = app.receipt();
    if (!receipt.empty()) {
        const float avail = std::max(1.0f, limit - x - tokens::spacePx(m, Space::Sm));
        const float w = std::min(avail, ui::measure(m, receipt, tier, false));
        ui::textAt(m, layout::Rect{limit - w, f.statusBar.y, w, f.statusBar.h},
                   ui::fitLine(m, receipt, w, tier, false),
                   studio_theme::field(&ThemeColors::statusRunning), tier, false, 0.0f, 0.5f);
    }
}

void drawSlash(App& app, const Frame& f) {
    const view_model::Screen& s = app.screen();
    if (!s.slashOpen) return;
    const Metrics& m = f.metrics;
    const Palette p = ui::palette();
    const layout::Rect box = f.slash;
    ImGui::SetNextWindowPos(ImVec2(box.x, box.y));
    ImGui::SetNextWindowSize(ImVec2(box.w, box.h));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, p.overlay);
    ImGui::PushStyleColor(ImGuiCol_Border, p.border);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(tokens::spacePx(m, Space::Md),
                                                           tokens::spacePx(m, Space::Sm)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, m.hairline);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, tokens::radiusPx(m, Radius::Md));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(0.0f, std::max(1.0f, tokens::spacePx(m, Space::Xs) * 0.5f)));
    // NoFocusOnAppearing / NoNavFocus: without them ImGui focuses a window the
    // moment it appears, and FocusWindow() drops the active item of every OTHER
    // window -- which is the composer. Typing "/" opened this list and took the
    // keyboard away, leaving nothing but Up/Down (handled by the app, not ImGui).
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                   ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNavFocus;
    if (ImGui::Begin("##slash", nullptr, flags)) {
        const int count = static_cast<int>(s.slashRows.size());
        const int shown = std::min(10, count);
        for (int i = s.slashFirst; i < count && i < s.slashFirst + shown; ++i) {
            const view_model::SlashRow& row = s.slashRows[static_cast<size_t>(i)];
            std::string text = row.name;
            if (!row.args.empty()) text += " " + row.args;
            text += "  ";
            text += row.desc;
            const std::string line =
                ui::fitLine(m, text, ImGui::GetContentRegionAvail().x, Type::Body, false);
            if (ui::listRow(m, line, i == s.slashSelected)) app.slashSelect(i);
        }
        ui::hint(m, "Enter = fill, Esc = close, Up/Down = move");
    }
    ImGui::End();
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(2);
}

} // namespace
} // namespace studio

namespace studio {
namespace {

// ---- frame ---------------------------------------------------------------

// Agent units packed into rows of equal cells. The cell is the widest unit, so an
// idle agent cannot shift its neighbours (the TUI's size(WIDTH, EQUAL) rule) and
// a long narration is ellipsized instead of wrapping the strip.
std::vector<std::vector<int>> packAgents(const Metrics& m,
                                         const std::vector<view_model::AgentRow>& agents,
                                         float avail, float* rowH) {
    std::vector<std::vector<int>> rows;
    const float gap = tokens::spacePx(m, Space::Xl);
    float unit = 180.0f * m.factor;
    for (const auto& a : agents) {
        const float want = ui::measure(m, a.tag, Type::Caption) + m.iconSize +
                          tokens::spacePx(m, Space::Xs) * 3.0f + 150.0f * m.factor;
        unit = std::max(unit, std::min(want, avail));
    }
    const int per = std::max(1, static_cast<int>((avail + gap) / (unit + gap)));
    for (int i = 0; i < static_cast<int>(agents.size()); i += per) {
        std::vector<int> row;
        for (int k = i; k < static_cast<int>(agents.size()) && k < i + per; ++k) row.push_back(k);
        rows.push_back(row);
    }
    if (rowH) *rowH = tokens::linePx(m, Type::Caption) + tokens::spacePx(m, Space::Xs);
    return rows;
}

// Visual rows the prompt needs for `text`, clamped to what the window may give up
// to a composer before the conversation disappears. The field is monospaced, so
// one row is one type tier and the count is a division, not a guess.
constexpr int kComposerMaxRows = 7;

int composerRowCount(const std::string& text, const Metrics& m, float composerW) {
    // Two rows by default: one line of typing plus the line you are about to
    // write is the difference between a search box and a prompt.
    if (text.empty()) return 2;
    const float innerW =
        std::max(1.0f, composerW - 2.0f * tokens::spacePx(m, Space::Md) - m.scrollbar);
    const fit::Measure measure = [&m](const std::string& s) {
        return ui::measure(m, s, Type::Mono, true);
    };
    const int rows = fit::wrappedRowCount(text, innerW, measure);
    return std::min(kComposerMaxRows, std::max(2, rows));
}

Frame computeFrame(App& app, const Metrics& m) {
    Frame f;
    f.metrics = m;
    const view_model::Screen& s = app.screen();
    const float page = m.pagePad;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    f.window = layout::Rect{vp->WorkPos.x, vp->WorkPos.y, vp->WorkSize.x, vp->WorkSize.y};
    f.dockOpen = s.dock;

    const float contentW = std::max(1.0f, f.window.w - 2.0f * page);
    const float gap = tokens::spacePx(m, Space::Sm);

    // --- measured content -------------------------------------------------
    const float titleH = tokens::linePx(m, Type::Title);
    // Row 0 holds the wordmark AND as many buttons as fit beside it; the rest wrap
    // into further rows, and the band grows by exactly what they need. The width is
    // the header's inner width (what drawHeader gets after its own inset), so the
    // packing and the drawing can never disagree about how much room there is.
    const float row0H = std::max(titleH, m.controlH);
    const float headerPad = tokens::spacePx(m, Space::Lg);
    g_headerPlan = planHeaderActions(app, m, std::max(1.0f, contentW - 2.0f * headerPad));
    const float extraActionRows = std::max(0.0f, g_headerPlan.height - m.controlH);

    float agentRowH = 0.0f;
    if (!s.agentsEmpty) f.headerAgentRows = packAgents(m, s.agents, contentW, &agentRowH);
    f.headerAgentRowH = agentRowH;
    const float agentH =
        f.headerAgentRows.empty() ? 0.0f : static_cast<float>(f.headerAgentRows.size()) * agentRowH;
    const float headerH = 2.0f * gap + row0H + extraActionRows + (agentH > 0.0f ? gap + agentH : 0.0f);

    // The status line moved into the composer's row (its own column on the right),
    // so the footer stack is now just: queued hint, prompt, service bar.
    const float statusLineH = tokens::linePx(m, Type::Body) + tokens::spacePx(m, Space::Xs);
    const float queuedH = s.queued > 0 ? statusLineH : 0.0f;
    // The prompt is one control, not a card with a field inside, and it is
    // multi-line: its height is the number of rows the text needs (wrap plus the
    // newlines typed with Shift+Enter), measured before anything is drawn so the
    // chat column gives up exactly that height on this frame.
    const float dockActual = f.dockOpen ? std::max(m.dockW, 220.0f * m.factor) : 0.0f;
    // The transcript column's width has to be known BEFORE the footer is measured,
    // because the prompt and the status line share that width. The left column used
    // to be subtracted later by the row solver, so this number was the whole
    // remaining window: the prompt then overhung its own column and the status
    // column came out negative and drew nothing.
    const float leftW =
        app.leftOpen() ? std::min(std::max(340.0f * m.factor, contentW * 0.26f),
                                  contentW * 0.40f)
                       : 0.0f;
    const float chatW = std::max(120.0f, contentW - (leftW > 0.0f ? leftW + m.gutter : 0.0f) -
                                               (f.dockOpen ? m.gutter + dockActual : 0.0f));
    // The prompt keeps a column on its right for the status line, so measure it
    // against the width it will actually get -- wrapping is computed from the
    // field's own inner width, not from the whole footer's.
    const float statusColW =
        std::min(chatW * 0.42f, std::max(200.0f * m.factor, chatW * 0.30f));
    const float promptW = std::max(120.0f, chatW - statusColW - gap);
    f.composerRows = composerRowCount(app.inputText(), m, promptW);
    const float promptRowH = tokens::typePx(m, Type::Mono);
    const float composerH = std::max(m.controlH, static_cast<float>(f.composerRows) * promptRowH +
                                                     2.0f * tokens::spacePx(m, Space::Sm));
    // The footer is read at arm's length: the status line and the service bar both
    // sit on the Body tier. They were Small (13.5 px), which reads as a caption.
    const float statusBarH = tokens::linePx(m, Type::Body) + 2.0f * tokens::spacePx(m, Space::Xs);
    // One rhythm for the whole footer: queued hint, prompt, service bar, separated
    // by the same step, so the three surfaces read as one stack.
    const float footerH = queuedH + composerH + gap + statusBarH;

    // --- the vertical stack: header, body (grows), footer ------------------
    layout::Node root = layout::Node::col(m.gutter);
    root.padding(layout::Edges::all(page));
    root.add(layout::Node::none().height(headerH));
    layout::Node body = layout::Node::row(m.gutter).hGrow(1.0f);
    // The left column is a sibling of the chat, not an overlay: the transcript
    // shrinks by exactly its width, and the reading measure re-centres in what is
    // left. `leftOpen` is the panel's own chevron, not a key. Its width was measured
    // above, because the footer needed it.
    if (leftW > 0.0f) body.add(layout::Node::none().width(leftW));
    // The footer belongs to the transcript, not to the window: the prompt and the
    // service bar are about the session in this column, so they sit INSIDE it and
    // the side panels keep their full height. A window-wide footer stole the bottom
    // strip from both columns and made the prompt look like a caption under the
    // whole screen.
    layout::Node mid = layout::Node::col(gap).wGrow(1.0f);
    mid.add(layout::Node::none().hGrow(1.0f));      // the transcript
    mid.add(layout::Node::none().height(footerH));  // its own prompt + service bar
    body.add(mid);
    if (f.dockOpen) body.add(layout::Node::none().width(m.dockW).minsize(220.0f * m.factor, 0.0f));
    root.add(body);
    layout::solve(root, f.window);

    f.header = root.children[0].rect;
    // The solver's own children, not a copy of their rects: the body node holds
    // the left column, the transcript+footer column and the dock, and all three
    // were placed against the height it got.
    const layout::Node& bodyNode = root.children[1];
    size_t child = 0;
    f.leftOpen = leftW > 0.0f;
    if (f.leftOpen) f.left = bodyNode.children[child++].rect;
    const layout::Node& midNode = bodyNode.children[child++];
    f.chat = midNode.children[0].rect;
    f.footer = midNode.children[1].rect;
    if (f.dockOpen && child < bodyNode.children.size()) f.dock = bodyNode.children[child].rect;
    // --- the left column's own stack: sessions / handle / files ----------------
    // The Sessions panel is as tall as its content (a minimal wrapper: one row per
    // pane plus the header) and the file tree gets every pixel that is left, capped
    // so five panes cannot eat the tree.
    if (f.leftOpen && !f.left.empty()) {
        const float gap = tokens::spacePx(m, Space::Sm);
        const float handleH = tokens::spacePx(m, Space::Sm);
        f.sessionsBarH = std::max(4.0f, m.hairline * 3.0f);
        // Title line + three ONE-LINE metrics: the old figure counted a caption
        // line AND a track per metric, which is why one card filled a third of the column.
        const float metricH = std::max(tokens::linePx(m, Type::Caption), f.sessionsBarH);
        f.sessionsRowH = m.rowH + 3.0f * (metricH + gap * 0.5f) + gap;
        const float sessionsWanted = m.rowH + static_cast<float>(app.paneCount()) * f.sessionsRowH +
                                     3.0f * gap;
        const float sessionsMax = std::max(6.0f * m.rowH, f.left.h * 0.45f);
        const float sessionsH = std::min(sessionsWanted, sessionsMax);
        layout::Node col = layout::Node::col(0.0f);
        col.add(layout::Node::none().height(sessionsH));
        col.add(layout::Node::none().height(handleH));
        col.add(layout::Node::none().hGrow(1.0f));
        layout::solve(col, f.left);
        f.leftSessions = col.children[0].rect;
        f.leftSplit = col.children[1].rect;
        f.leftFiles = col.children[2].rect;
    }
    // The reading column: capped and centred. Published so the composer, the
    // slash menu and the modals share one left edge with the transcript.
    const float chatInner = std::max(120.0f, f.chat.w - 2.0f * gap - m.scrollbar);
    const float colW = std::min(chatInner, m.readingMax);
    f.chatColumn = layout::Rect{f.chat.x + (f.chat.w - colW) * 0.5f, f.chat.y, colW, f.chat.h};

    // --- the footer stack, top to bottom ----------------------------------
    float y = f.footer.y;
    if (queuedH > 0.0f) {
        f.queued = layout::Rect{f.footer.x, y, f.footer.w, queuedH};
        y += queuedH;
    }
    // The prompt keeps the transcript's left edge, and the status line takes the
    // column beside it: the two share one row, so the footer is two bands instead
    // of three, and the prompt gets the taller box the extra row buys.
    f.composer = layout::Rect{f.footer.x, y, promptW, composerH};
    f.statusLine =
        layout::Rect{f.footer.x + promptW + gap, y, f.footer.w - promptW - gap, composerH};
    y += composerH + gap;
    f.statusBar = layout::Rect{f.footer.x, y, f.footer.w, statusBarH};
    // --- the floating "/" menu, anchored above the composer ----------------
    if (s.slashOpen) {        const int shown = std::min(10, static_cast<int>(s.slashRows.size()));
        const float hintH = tokens::linePx(m, Type::Small) + tokens::spacePx(m, Space::Xs);
        // The item gap is part of the height: counting only the rows left the hint
        // line under them clipped by the window's own bottom edge.
        const float rowGap = std::max(1.0f, tokens::spacePx(m, Space::Xs) * 0.5f);
        const float slashH = static_cast<float>(shown) * (m.rowH + rowGap) + hintH +
                             tokens::spacePx(m, Space::Md) + 2.0f * m.hairline;
        const float slashW = std::min(f.composer.w, 560.0f * m.factor);
        // Anchored to the prompt's left edge, not to the reading column: the prompt
        // is the surface it belongs to, so the two outlines share one line.
        f.slash = layout::Rect{f.composer.x, f.composer.y - gap - slashH, slashW, slashH};
    }
    f.headerAgentRowsY = f.header.y + gap + row0H + extraActionRows + gap;
    return f;
}

} // namespace

const Frame& lastFrame() {
    return g_frame;
}

void drawFrame(App& app) {
    // Re-applied every frame: a runtime theme switch recolours the text but would
    // leave every frame, button, header and scrollbar on the startup palette
    // (the "theme colours are broken" report). It is a few dozen writes.
    studio_theme::applyStyle();
    const Metrics m = ui::metricsFor(app.dpiScale());
    studio_theme::applyMetrics(m);
    ui::setZoom(ui::zoom()); // re-assert FontScaleMain; metrics already cover it

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    // The window itself has no padding: every band is placed by the frame, which
    // already accounts for the page margin. Two sources of margin is how the old
    // window ended up with a 6 px edge on one side and 16 on the other.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("##projv", nullptr, flags)) {
        g_frame = computeFrame(app, m);
        const Palette p = ui::palette();
        ui::fill(g_frame.window, p.window);
        drawHeader(app, g_frame);
        if (g_frame.leftOpen) drawLeftBar(app, g_frame);
        drawChat(app, g_frame);
        if (g_frame.dockOpen) drawDock(app, g_frame);
        // After the prompt, not before: the prompt paints its own filled box and a
        // frame, and anything drawn earlier in the same row is covered by it.
        drawQueued(app, g_frame);
        drawPrompt(app, g_frame);
        drawStatusLine(app, g_frame);
        drawStatusBar(app, g_frame);
        drawSlash(app, g_frame);
    }
    ImGui::End();
    ImGui::PopStyleVar();
    drawModals(app);
}

} // namespace studio
