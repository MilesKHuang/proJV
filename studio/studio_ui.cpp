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
#include "design/fit.h"
#include "design/layout.h"
#include "design/tokens.h"
#include "studio_app.h"
#include "studio_input.h"
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
const char* const kSpin[4] = {"|", "/", "-", "\\"};

int spinPhase() {
    return (ImGui::GetFrameCount() / 8) % 4;
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
    if (!row.extra.empty())
        g_surface.drawBlock(blockId + 3, row.extra, false, textW, fld(row.extraColor));
}

void drawChatRow(const Frame& f, const view_model::ChatRow& row, float width, int index) {
    const Metrics& m = f.metrics;
    const float padX = tokens::spacePx(m, Space::Md);
    const float padY = tokens::spacePx(m, Space::Sm);
    // A row that carries a label is a message: it gets a card. A bare text row is
    // a continuation of the one above it and stays on the surface.
    const bool carded = !row.label.empty() || row.kind == "tool" || row.kind == "result" ||
                        row.kind == "diff" || row.kind == "system";
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
            if (!row.label.empty()) {
                ui::text(m, displayLabel(row.label), labelAccent(row), Type::Caption);
                ImGui::Dummy(ImVec2(1.0f, tokens::spacePx(m, Space::Xs)));
            }
            drawRowBody(m, row, index * 10, false);
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    } else {
        drawRowBody(m, row, index * 10, false);
    }
    ImGui::PopID();
    ImGui::Dummy(ImVec2(1.0f, tokens::spacePx(m, Space::Xs)));
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
        for (int i = 0; i < static_cast<int>(s.chat.size()); ++i)
            drawChatRow(f, s.chat[static_cast<size_t>(i)], colW, i);
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
        // Mouse selection and Ctrl+C over everything drawn above.
        g_surface.handleInput();
        if (inset > 0.0f) ImGui::Unindent(inset);
    }
    // Sticky bottom: follow new content only while the view sits at the bottom.
    // Scrolling up stops the chase on the very frame the wheel turns, and coming
    // back resumes it -- the return value IS the next frame's state.
    const bool follow = open && app.takeFollowBottom();
    const bool atBottom = ui::endScroll(follow);
    if (open) app.setFollowBottom(atBottom);
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
        ui::textAt(m, layout::Rect{rest.x, rest.y, m.iconSize + pad, rest.h}, kSpin[spinPhase()],
                   glyph, Type::Caption, true, 0.0f);
        rest.x += m.iconSize + pad;
        rest.w -= m.iconSize + pad;
    }
    if (rest.w > 0.0f) {
        ui::textAt(m, rest,
                   ui::fitLine(m, a.detail, rest.w, Type::Caption, false), p.textDim, Type::Caption,
                   false, 0.0f);
    }
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

    // The actions are measured FIRST, so the title knows exactly what it has left.
    // This is the responsive part: as the window narrows, the least important
    // action drops out instead of the header overflowing its band.
    struct Action {
        std::string label;
        GKey key;
        ui::Btn kind;
        const char* hint; // the shortcut, shown on hover instead of in a keycap bar
    };
    std::vector<Action> wanted;
    wanted.push_back({"Model", GKey::F10, ui::Btn::Secondary, "F10  -  switch model"});
    wanted.push_back({"Sessions", GKey::F4, ui::Btn::Secondary, "F4  -  open a saved session"});
    wanted.push_back({"Config", GKey::F2, ui::Btn::Secondary, "F2  -  configuration"});
    wanted.push_back({"MCP", GKey::F5, ui::Btn::Secondary, "F5  -  MCP servers"});
    wanted.push_back({s.dock ? "Dock on" : "Dock off", GKey::F9, ui::Btn::Ghost,
                      "F9  -  show or hide the TODO dock"});
    wanted.push_back({"Help", GKey::F1, ui::Btn::Secondary, "F1  -  every key and command"});

    const float gap = tokens::spacePx(m, Space::Sm);
    std::vector<Action> shown;
    float actionsW = 0.0f;
    const float maxActions = row.w * 0.62f;
    for (const auto& a : wanted) {
        const float w = ui::buttonWidth(m, a.label, a.kind);
        if (actionsW + w + gap > maxActions) break;
        shown.push_back(a);
        actionsW += w + gap;
    }
    if (!shown.empty()) actionsW -= gap;

    float right = row.right();
    for (auto it = shown.rbegin(); it != shown.rend(); ++it) {
        const float w = ui::buttonWidth(m, it->label, it->kind);
        const layout::Rect box{right - w, row.y + (titleH - m.controlH) * 0.5f, w, m.controlH};
        if (ui::buttonAt(m, box, it->label, it->kind, it->hint)) app.handleKey(it->key);
        right -= w + gap;
    }
    const float leftW = std::max(0.0f, (shown.empty() ? row.w : right - row.x) - gap);

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
    const layout::Rect titleBox{row.x, row.y, titleW, titleH};
    ui::textAt(m, titleBox, ui::fitLine(m, titleText, titleW, titleTier, false), p.text,
               titleTier, false, 0.0f, 0.5f);
    if (showPills) {
        float x = titleBox.right() + tokens::spacePx(m, Space::Lg);
        const float pillH =
            tokens::linePx(m, Type::Caption) + 2.0f * tokens::spacePx(m, Space::Xs);
        const float pillY = row.y + (titleH - pillH) * 0.5f;
        ui::badge(m, layout::Rect{x, pillY, tagW, pillH}, tag, p.textDim, p.surface);
        x += tagW + tokens::spacePx(m, Space::Lg);
        const ImU32 phaseBg = fld(s.phaseColor);
        ui::badge(m, layout::Rect{x, pillY, phaseW, pillH},
                  ui::fitLine(m, phase, phaseW - tokens::spacePx(m, Space::Sm), Type::Caption,
                              false),
                  ui::contrastOn(phaseBg), phaseBg);
        x += phaseW + tokens::spacePx(m, Space::Xs);
        if (s.busy) {
            ui::textAt(m, layout::Rect{x, pillY, m.iconSize, pillH}, kSpin[spinPhase()], p.accent,
                       Type::Caption, true, 0.5f);
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
    if (s.statusSpinner) {
        const float w = tokens::spacePx(m, Space::Md);
        ui::textAt(m, layout::Rect{box.x, box.y, w, box.h}, kSpin[spinPhase()], p.accent,
                   Type::Small, true, 0.0f);
        box.x += w + tokens::spacePx(m, Space::Xs);
        box.w -= w + tokens::spacePx(m, Space::Xs);
    }
    if (!s.skillStatus.empty()) {
        const std::string pill = "[skill] " + s.skillStatus;
        const float w = std::min(box.w, ui::measure(m, pill, Type::Caption, false) +
                                            2.0f * tokens::spacePx(m, Space::Sm));
        const float h = std::min(box.h, tokens::linePx(m, Type::Caption) +
                                            2.0f * tokens::spacePx(m, Space::Xs));
        const layout::Rect pillBox{box.right() - w, box.y + (box.h - h) * 0.5f, w, h};
        ui::badge(m, pillBox, ui::fitLine(m, pill, pillBox.w - tokens::spacePx(m, Space::Md),
                                          Type::Caption, false),
                  ui::contrastOn(p.warning), p.warning);
        box.w -= w + tokens::spacePx(m, Space::Sm);
    }
    const Type tiers[2] = {Type::Small, Type::Caption};
    ui::fittedText(m, box, s.statusText, fld(s.statusColor), tiers, 2, false, 0.0f);
}

void drawQueued(App& app, const Frame& f) {
    if (app.screen().queued <= 0) return;
    const Metrics& m = f.metrics;
    const view_model::Screen& s = app.screen();
    const std::string text =
        "Queued: " + std::to_string(s.queued) + " message(s) - sends after current turn";
    ui::textAt(m, f.queued, text, studio_theme::field(&ThemeColors::todoTitle), Type::Small, false,
               0.0f);
}

void drawStatusBar(App& app, const Frame& f) {
    const Metrics& m = f.metrics;
    const Palette p = ui::palette();
    const view_model::Screen& s = app.screen();
    ui::fill(f.statusBar, p.surfaceAlt);
    const float gap = tokens::spacePx(m, Space::Sm);
    float x = f.statusBar.x + tokens::spacePx(m, Space::Sm);
    const float limit = f.statusBar.right() - tokens::spacePx(m, Space::Sm);
    for (size_t i = 0; i < s.statusBar.size(); ++i) {
        const view_model::Segment& seg = s.statusBar[i];
        const float w = ui::measure(m, seg.text, Type::Small, false);
        if (x + w > limit) break;
        if (i > 0) {
            const float sepW = ui::measure(m, "  |  ", Type::Small, false);
            if (x + sepW + w > limit) break;
            ui::textAt(m, layout::Rect{x, f.statusBar.y, sepW, f.statusBar.h}, "  |  ", p.divider,
                       Type::Small, false, 0.0f);
            x += sepW;
        }
        ui::textAt(m, layout::Rect{x, f.statusBar.y, w, f.statusBar.h}, seg.text, fld(seg.color),
                   Type::Small, false, 0.0f);
        x += w + gap * 0.5f;
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
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
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
    float agentRowH = 0.0f;
    if (!s.agentsEmpty) f.headerAgentRows = packAgents(m, s.agents, contentW, &agentRowH);
    f.headerAgentRowH = agentRowH;
    const float agentH =
        f.headerAgentRows.empty() ? 0.0f : static_cast<float>(f.headerAgentRows.size()) * agentRowH;
    const float headerH = 2.0f * gap + titleH + (agentH > 0.0f ? gap + agentH : 0.0f);

    const float statusLineH = tokens::linePx(m, Type::Small) + tokens::spacePx(m, Space::Xs);
    const float queuedH = s.queued > 0 ? statusLineH : 0.0f;
    // The prompt is one control, not a card with a field inside: its height is the
    // token control height, and the frame draws the only border it has.
    const float composerH = std::max(m.controlH, tokens::linePx(m, Type::Mono) +
                                                    2.0f * tokens::spacePx(m, Space::Md));
    // The service bar is reference data, not a footnote: it reads at the Small tier,
    // not at Caption. "The status bar text is too small" was the report, and it was
    // right -- 12.5 px of grey is a caption, not a status.
    const float statusBarH = tokens::linePx(m, Type::Small) + 2.0f * tokens::spacePx(m, Space::Xs);
    // One rhythm for the whole footer: status line, prompt, service bar, all separated
    // by the same step, so the three surfaces read as one stack instead of three boxes
    // that happen to be nearby.
    const float footerH = statusLineH + queuedH + gap + composerH + gap + statusBarH + gap;

    // --- the vertical stack: header, body (grows), footer ------------------
    layout::Node root = layout::Node::col(m.gutter);
    root.padding(layout::Edges::all(page));
    root.add(layout::Node::none().height(headerH));
    layout::Node body = layout::Node::row(m.gutter).hGrow(1.0f);
    body.add(layout::Node::none().wGrow(1.0f));
    if (f.dockOpen) body.add(layout::Node::none().width(m.dockW).minsize(220.0f * m.factor, 0.0f));
    root.add(body);
    root.add(layout::Node::none().height(footerH));
    layout::solve(root, f.window);

    f.header = root.children[0].rect;
    // The solver's own children, not a copy of their rects: the body node holds
    // the chat and the dock, and both were placed against the height it got.
    const layout::Node& bodyNode = root.children[1];
    f.chat = bodyNode.children[0].rect;
    if (f.dockOpen && bodyNode.children.size() > 1) f.dock = bodyNode.children[1].rect;
    f.footer = root.children[2].rect;
    // The reading column: capped and centred. Published so the composer, the
    // slash menu and the modals share one left edge with the transcript.
    const float chatInner = std::max(120.0f, f.chat.w - 2.0f * gap - m.scrollbar);
    const float colW = std::min(chatInner, m.readingMax);
    f.chatColumn = layout::Rect{f.chat.x + (f.chat.w - colW) * 0.5f, f.chat.y, colW, f.chat.h};

    // --- the footer stack, top to bottom ----------------------------------
    float y = f.footer.y;
    f.statusLine = layout::Rect{f.footer.x, y, f.footer.w, statusLineH};
    y += statusLineH;
    if (queuedH > 0.0f) {
        f.queued = layout::Rect{f.footer.x, y, f.footer.w, queuedH};
        y += queuedH;
    }
    y += gap;
    // The prompt shares the transcript's edges, not a narrower box of its own: an
    // outline that does not line up with the card above it reads as a mistake, which
    // is exactly what "the input box looks detached from the background" was.
    f.composer = layout::Rect{f.chat.x, y, f.chat.w, composerH};
    y += composerH + gap;
    f.statusBar = layout::Rect{f.footer.x, y, f.footer.w, statusBarH};
    y += statusBarH + gap;
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
    f.headerAgentRowsY = f.header.y + gap + titleH + gap;
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
        drawChat(app, g_frame);
        if (g_frame.dockOpen) drawDock(app, g_frame);
        drawStatusLine(app, g_frame);
        drawQueued(app, g_frame);
        drawPrompt(app, g_frame);
        drawStatusBar(app, g_frame);
        drawSlash(app, g_frame);
    }
    ImGui::End();
    ImGui::PopStyleVar();
    drawModals(app);
}

} // namespace studio
