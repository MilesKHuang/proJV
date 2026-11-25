// proJV studio -- the widget kit (see ui.h).
#include "ui.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "imgui_stdlib.h"

#include "studio_theme.h"
#include "tui/theme_manager.h"

namespace studio {
namespace ui {
namespace {

float g_zoom = 1.0f;
bool g_fieldEdited = false;
bool g_rowDouble = false;

int typeIdx(Type t) {
    const int i = static_cast<int>(t);
    return (i < 0 || i >= tokens::kTypeCount) ? static_cast<int>(Type::Body) : i;
}

ImVec2 v2(const Rect& r) {
    return ImVec2(r.x, r.y);
}

// A widget's identity inside one window: its label plus where it sits. Two
// buttons with the same label in the same window still differ by position, and
// the position is stable across frames, so the id is too.
void pushWidgetId(const std::string& label, const Rect& box) {
    ImGui::PushID(label.c_str());
    ImGui::PushID(static_cast<int>(box.x));
    ImGui::PushID(static_cast<int>(box.y));
}

void popWidgetId() {
    ImGui::PopID();
    ImGui::PopID();
    ImGui::PopID();
}

float labelWidth(const std::string& label, Type tier) {
    ImFont* face = studio_theme::uiFont();
    if (!face) return 0.0f;
    ImGui::PushFont(face, tokens::kTypePx[typeIdx(tier)]);
    const float w = ImGui::CalcTextSize(label.c_str()).x;
    ImGui::PopFont();
    return w;
}

void paintLabel(const std::string& label, const Rect& box, ImU32 color, Type tier) {
    ImFont* face = studio_theme::uiFont();
    if (!face) return;
    ImGui::PushFont(face, tokens::kTypePx[typeIdx(tier)]);
    const ImVec2 size = ImGui::CalcTextSize(label.c_str());
    const float x = box.x + (box.w - size.x) * 0.5f;
    const float y = box.y + (box.h - size.y) * 0.5f;
    ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                                        ImVec2(std::floor(x), std::floor(y)), color, label.c_str());
    ImGui::PopFont();
}

} // namespace

// Black or white, whichever the fill is not: a label on the accent has to stay
// legible on every theme, including a light one whose accent is pale.
ImU32 contrastOn(ImU32 bg) {
    const float lum =
        0.2126f * ((bg >> IM_COL32_R_SHIFT) & 0xFF) + 0.7152f * ((bg >> IM_COL32_G_SHIFT) & 0xFF) +
        0.0722f * ((bg >> IM_COL32_B_SHIFT) & 0xFF);
    return lum < 150.0f ? IM_COL32(255, 255, 255, 255) : IM_COL32(17, 17, 17, 255);
}

float measure(const Metrics& m, const std::string& s, Type tier, bool mono) {
    (void)m;
    if (s.empty()) return 0.0f;
    ImFont* face = mono ? studio_theme::monoFont() : studio_theme::uiFont();
    if (!face) return 0.0f;
    ImGui::PushFont(face, tokens::kTypePx[typeIdx(tier)]);
    const float w = ImGui::CalcTextSize(s.c_str()).x;
    ImGui::PopFont();
    return w;
}

// ---- palette --------------------------------------------------------------

Palette palette() {
    const ThemeColors& T = ThemeManager::instance().current();
    const ImU32 window = studio_theme::fromHex(T.windowBg);
    const ImU32 accent = studio_theme::fromHex(T.phaseStreaming);

    Palette p;
    p.window = window;
    // Three surfaces from one background: every theme ships a window colour and a
    // popup colour, but no "one step up" shade, so the levels are derived. These
    // three percentages are the whole elevation system.
    p.surface = studio_theme::surface(window, 0.05f);
    p.surfaceAlt = studio_theme::surface(window, 0.09f);
    p.overlay = studio_theme::field(&ThemeColors::popupBg);
    p.border = studio_theme::field(&ThemeColors::border);
    p.divider = studio_theme::field(&ThemeColors::separator);

    p.text = studio_theme::field(&ThemeColors::text);
    // Secondary text goes through the contrast guard: a theme's "disabled" grey is
    // authored for a terminal and is often 2.7:1 on this background.
    p.textDim = studio_theme::readable(studio_theme::field(&ThemeColors::textDisabled), window);
    p.textOnAccent = contrastOn(accent);

    p.accent = accent;
    p.accentHover = studio_theme::field(&ThemeColors::phaseExecutingTools);
    p.accentPressed = studio_theme::field(&ThemeColors::buttonActive);
    p.hover = studio_theme::surface(window, 0.13f);
    p.selected = studio_theme::mix(window, accent, 0.22f);
    p.pressed = studio_theme::surface(window, 0.20f);

    p.danger = studio_theme::field(&ThemeColors::statusError);
    p.success = studio_theme::field(&ThemeColors::todoDone);
    p.warning = studio_theme::field(&ThemeColors::phaseAwaitApproval);
    p.info = accent;

    p.scrollTrack = studio_theme::surface(window, 0.03f);
    p.scrollThumb = studio_theme::field(&ThemeColors::scrollbarGrab);
    p.inputBg = studio_theme::surface(window, 0.03f);
    return p;
}

// ---- metrics --------------------------------------------------------------

void setZoom(float z) {
    if (!(z > 0.0f)) z = 1.0f;
    g_zoom = std::min(3.0f, std::max(0.5f, z));
    // ImGui applies FontScaleMain to every pushed size, so the zoom has to live
    // there as well: a pushed size is the 100% base, not the final pixel count.
    ImGui::GetStyle().FontScaleMain = g_zoom;
}

float zoom() {
    return g_zoom;
}

Metrics metricsFor(float dpi) {
    return tokens::build(dpi, g_zoom);
}

// ---- text -----------------------------------------------------------------

void pushType(const Metrics& m, Type tier, bool mono) {
    (void)m;
    ImFont* face = mono ? studio_theme::monoFont() : studio_theme::uiFont();
    if (!face) return;
    ImGui::PushFont(face, tokens::kTypePx[typeIdx(tier)]);
}

void popType() {
    ImGui::PopFont();
}

float textWidth(const std::string& utf8) {
    if (utf8.empty()) return 0.0f;
    return ImGui::CalcTextSize(utf8.c_str()).x;
}

fit::Measure measurer() {
    return [](const std::string& s) { return textWidth(s); };
}

fit::UnitMeasure unitMeasurer() {
    return [](const std::string& s) {
        const float size = ImGui::GetFontSize();
        if (size <= 0.0f) return 0.0f;
        return textWidth(s) / size;
    };
}

void text(const Metrics& m, const std::string& s, ImU32 color, Type tier, bool mono) {
    if (s.empty()) return;
    pushType(m, tier, mono);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(s.c_str());
    ImGui::PopStyleColor();
    popType();
}

void paragraph(const Metrics& m, const std::string& s, ImU32 color, Type tier) {
    if (s.empty()) return;
    pushType(m, tier, false);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextWrapped("%s", s.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    popType();
}

void hint(const Metrics& m, const std::string& s) {
    if (s.empty()) return;
    const Palette p = palette();
    pushType(m, Type::Small, false);
    ImGui::PushStyleColor(ImGuiCol_Text, p.textDim);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextWrapped("%s", s.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    popType();
}

void textAt(const Metrics& m, const Rect& box, const std::string& s, ImU32 color, Type tier,
            bool mono, float alignX, float alignY) {
    if (box.empty() || s.empty()) return;
    pushType(m, tier, mono);
    const ImVec2 size = ImGui::CalcTextSize(s.c_str());
    const float x = box.x + (box.w - size.x) * std::min(1.0f, std::max(0.0f, alignX));
    const float y = box.y + (box.h - size.y) * std::min(1.0f, std::max(0.0f, alignY));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(v2(box), ImVec2(box.right(), box.bottom()), true);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(std::floor(x), std::floor(y)), color,
                s.c_str());
    dl->PopClipRect();
    popType();
}

std::string fitLine(const Metrics& m, const std::string& s, float width, Type tier, bool mono) {
    if (s.empty() || width <= 0.0f) return std::string();
    pushType(m, tier, mono);
    std::string out = fit::ellipsize(s, width, measurer());
    popType();
    return out;
}

void fittedText(const Metrics& m, const Rect& box, const std::string& s, ImU32 color,
                const Type* tiers, int tierCount, bool mono, float alignX) {
    if (box.empty() || s.empty() || tiers == nullptr || tierCount <= 0) return;

    const int n = std::min(tierCount, 8);
    // Measure once at the smallest tier and scale from there: two sizes of one
    // face have a linear width ratio, so a single measurement picks the tier.
    pushType(m, tiers[n - 1], mono);
    const fit::UnitMeasure unit = unitMeasurer();
    popType();

    float sizes[8] = {0.0f};
    for (int i = 0; i < n; ++i) sizes[i] = tokens::typePx(m, tiers[i]);
    const int idx = fit::bestTier(s, box.w, sizes, n, unit);
    const Type chosen = tiers[std::min(idx, n - 1)];

    pushType(m, chosen, mono);
    const std::string shown = fit::ellipsize(s, box.w, measurer());
    popType();
    textAt(m, box, shown, color, chosen, mono, alignX, 0.5f);
}

// ---- surfaces -------------------------------------------------------------

void fill(const Rect& box, ImU32 color) {
    if (box.empty()) return;
    ImGui::GetWindowDrawList()->AddRectFilled(v2(box), ImVec2(box.right(), box.bottom()), color);
}

void fillRound(const Rect& box, ImU32 color, float radius) {
    if (box.empty()) return;
    ImGui::GetWindowDrawList()->AddRectFilled(v2(box), ImVec2(box.right(), box.bottom()), color,
                                              radius);
}

void stroke(const Rect& box, ImU32 color, float thickness, float radius) {
    if (box.empty()) return;
    ImGui::GetWindowDrawList()->AddRect(v2(box), ImVec2(box.right(), box.bottom()), color, radius,
                                        0, thickness);
}

void card(const Rect& box, const Metrics& m, bool raised) {
    if (box.empty()) return;
    const Palette p = palette();
    fillRound(box, raised ? p.surfaceAlt : p.surface, tokens::radiusPx(m, Radius::Md));
    stroke(box, p.border, m.hairline, tokens::radiusPx(m, Radius::Md));
}

void divider(const Metrics& m) {
    const Palette p = palette();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::GetWindowDrawList()->AddLine(pos, ImVec2(pos.x + w, pos.y), p.divider, m.hairline);
    ImGui::Dummy(ImVec2(1.0f, tokens::spacePx(m, Space::Md)));
}

void dividerAt(const Metrics& m, const Rect& box) {
    if (box.empty()) return;
    const Palette p = palette();
    const float y = box.y + box.h * 0.5f;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(box.x, y), ImVec2(box.right(), y), p.divider,
                                        m.hairline);
}

// ---- controls -------------------------------------------------------------

float buttonWidth(const Metrics& m, const std::string& label, Btn kind) {
    const float textW = labelWidth(label, Type::Body);
    const float pad = tokens::spacePx(m, Space::Lg);
    const float minW = (kind == Btn::Ghost ? 64.0f : 88.0f) * m.factor;
    return std::max(minW, textW + 2.0f * pad);
}

bool buttonAt(const Metrics& m, const Rect& box, const std::string& label, Btn kind,
              const char* tooltip) {
    if (box.empty()) return false;
    const Palette p = palette();
    ImGui::SetCursorScreenPos(v2(box));
    pushWidgetId(label, box);
    const bool clicked = ImGui::InvisibleButton("##btn", ImVec2(box.w, box.h));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    // The tooltip is delayed on purpose: an immediately-shown tooltip window under
    // the cursor competes with the button for hover, and a click could end up with
    // no item hovered on the release frame -- the button highlighted but dead.
    if (hovered && tooltip && *tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip("%s", tooltip);
    popWidgetId();

    ImU32 bg = 0;
    ImU32 fg = p.text;
    switch (kind) {
    case Btn::Primary:
        bg = held ? p.accentPressed : (hovered ? p.accentHover : p.accent);
        fg = contrastOn(bg);
        break;
    case Btn::Danger:
        bg = held ? studio_theme::mix(p.danger, IM_COL32(0, 0, 0, 255), 0.25f)
                  : (hovered ? studio_theme::mix(p.danger, IM_COL32(255, 255, 255, 255), 0.12f)
                             : p.danger);
        fg = contrastOn(bg);
        break;
    case Btn::Secondary:
        bg = held ? p.pressed : (hovered ? p.hover : p.surfaceAlt);
        break;
    case Btn::Ghost:
        bg = held ? p.pressed : (hovered ? p.hover : 0);
        break;
    }

    const float radius = tokens::radiusPx(m, Radius::Sm);
    if (bg != 0) fillRound(box, bg, radius);
    if (kind == Btn::Secondary) stroke(box, p.border, m.hairline, radius);
    paintLabel(label, box, fg, Type::Body);
    return clicked;
}

bool button(const Metrics& m, const std::string& label, Btn kind, const char* tooltip) {
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const Rect box{pos.x, pos.y, buttonWidth(m, label, kind), m.controlH};
    return buttonAt(m, box, label, kind, tooltip);
}

bool iconButton(const Metrics& m, const Rect& box, const std::string& glyph) {
    const Palette p = palette();
    ImGui::SetCursorScreenPos(v2(box));
    pushWidgetId(glyph, box);
    const bool clicked = ImGui::InvisibleButton("##icon", ImVec2(box.w, box.h));
    const bool hovered = ImGui::IsItemHovered();
    popWidgetId();
    if (hovered) fillRound(box, p.hover, tokens::radiusPx(m, Radius::Sm));
    paintLabel(glyph, box, p.textDim, Type::Body);
    return clicked;
}

bool checkbox(const Metrics& m, const std::string& label, bool* value) {
    const Palette p = palette();
    const float side = m.iconSize;
    const float gap = tokens::spacePx(m, Space::Sm);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const Rect area{pos.x, pos.y, side + gap + labelWidth(label, Type::Body), m.rowH};
    ImGui::SetCursorScreenPos(v2(area));
    pushWidgetId(label, area);
    const bool clicked = ImGui::InvisibleButton("##check", ImVec2(area.w, area.h));
    const bool hovered = ImGui::IsItemHovered();
    popWidgetId();

    const Rect strip{area.x, area.y + (area.h - side) * 0.5f, side, side};
    const float radius = tokens::radiusPx(m, Radius::Xs);
    const bool on = value && *value;
    fillRound(strip, on ? p.accent : (hovered ? p.hover : p.inputBg), radius);
    stroke(strip, on ? p.accent : p.border, m.hairline, radius);
    if (on) {
        // Drawn, not typed: a glyph would be a second visual style for one mark.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 mark = contrastOn(p.accent);
        const float t = std::max(1.5f, m.hairline * 1.5f);
        dl->AddLine(ImVec2(strip.x + side * 0.24f, strip.y + side * 0.52f),
                    ImVec2(strip.x + side * 0.44f, strip.y + side * 0.72f), mark, t);
        dl->AddLine(ImVec2(strip.x + side * 0.44f, strip.y + side * 0.72f),
                    ImVec2(strip.x + side * 0.78f, strip.y + side * 0.30f), mark, t);
    }
    const Rect textBox{strip.right() + gap, area.y, area.w - side - gap, area.h};
    textAt(m, textBox, label, p.text, Type::Body, false, 0.0f, 0.5f);
    if (clicked && value) *value = !*value;
    return clicked;
}

bool radio(const Metrics& m, const std::string& label, bool selected) {
    const Palette p = palette();
    const float side = m.iconSize;
    const float gap = tokens::spacePx(m, Space::Sm);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const Rect area{pos.x, pos.y, side + gap + labelWidth(label, Type::Body), m.rowH};
    ImGui::SetCursorScreenPos(v2(area));
    pushWidgetId(label, area);
    const bool clicked = ImGui::InvisibleButton("##radio", ImVec2(area.w, area.h));
    const bool hovered = ImGui::IsItemHovered();
    popWidgetId();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(area.x + side * 0.5f, area.y + area.h * 0.5f);
    const float r = side * 0.5f;
    dl->AddCircleFilled(c, r, hovered && !selected ? p.hover : p.inputBg);
    dl->AddCircle(c, r, selected ? p.accent : p.border, 0, m.hairline);
    if (selected) dl->AddCircleFilled(c, r * 0.45f, p.accent);
    const Rect textBox{area.x + side + gap, area.y, area.w - side - gap, area.h};
    textAt(m, textBox, label, p.text, Type::Body, false, 0.0f, 0.5f);
    return clicked;
}

int segmented(const Metrics& m, const std::vector<std::string>& options, int selected,
              float width) {
    if (options.empty() || width <= 0.0f) return -1;
    const Palette p = palette();
    const float radius = tokens::radiusPx(m, Radius::Sm);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const Rect row{pos.x, pos.y, width, m.controlH};
    fillRound(row, p.inputBg, radius);
    stroke(row, p.border, m.hairline, radius);

    const float cellW = row.w / static_cast<float>(options.size());
    int chosen = -1;
    for (int i = 0; i < static_cast<int>(options.size()); ++i) {
        const Rect cell{row.x + cellW * static_cast<float>(i), row.y, cellW, row.h};
        ImGui::SetCursorScreenPos(ImVec2(cell.x, cell.y));
        pushWidgetId(options[static_cast<size_t>(i)], cell);
        const bool clicked = ImGui::InvisibleButton("##seg", ImVec2(cell.w, cell.h));
        const bool hovered = ImGui::IsItemHovered();
        popWidgetId();
        const bool on = i == selected;
        if (on) {
            fillRound(cell.shrink(m.hairline, m.hairline), p.accent, radius);
        } else if (hovered) {
            fillRound(cell.shrink(m.hairline, m.hairline), p.hover, radius);
        }
        paintLabel(options[static_cast<size_t>(i)], cell, on ? contrastOn(p.accent) : p.text,
                   Type::Small);
        if (clicked) chosen = i;
    }
    // Leave the caller's cursor below the row: the cells moved it around.
    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y));
    ImGui::Dummy(ImVec2(width, m.controlH));
    return chosen;
}

bool field(const Metrics& m, const char* id, std::string* text, const char* hintText,
           bool password, float width) {
    if (!text) return false;
    const Palette p = palette();
    const float lineH = tokens::linePx(m, Type::Body);
    const float padY = std::max(0.0f, (m.controlH - lineH) * 0.5f - m.hairline);

    ImGui::PushStyleColor(ImGuiCol_FrameBg, p.inputBg);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, p.hover);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, p.surfaceAlt);
    ImGui::PushStyleColor(ImGuiCol_Text, p.text);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, p.textDim);
    ImGui::PushStyleColor(ImGuiCol_Border, p.border);
    ImGui::PushStyleColor(ImGuiCol_NavCursor, p.accent);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, tokens::radiusPx(m, Radius::Sm));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(tokens::spacePx(m, Space::Md), padY));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, m.hairline);
    pushType(m, Type::Body, false);

    ImGui::SetNextItemWidth(width < 0.0f ? -1.0f : width);
    ImGuiInputTextFlags flags =
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_EscapeClearsAll;
    if (password) flags |= ImGuiInputTextFlags_Password;
    const bool entered = ImGui::InputTextWithHint(id, hintText ? hintText : "", text, flags);
    g_fieldEdited = ImGui::IsItemEdited();

    popType();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(7);
    return entered;
}

bool fieldEdited() {
    return g_fieldEdited;
}

bool listRow(const Metrics& m, const std::string& label, bool selected, bool disabled) {
    const Palette p = palette();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const Rect row{pos.x, pos.y, w, m.rowH};
    ImGui::SetCursorScreenPos(v2(row));
    pushWidgetId(label, row);
    const bool clicked = ImGui::InvisibleButton("##row", ImVec2(row.w, row.h));
    const bool hovered = ImGui::IsItemHovered();
    g_rowDouble = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    popWidgetId();

    const float radius = tokens::radiusPx(m, Radius::Xs);
    if (selected) {
        fillRound(row, p.selected, radius);
        // A 2 px accent bar on the leading edge: the selected row stays visible
        // when the palette's selection tint is close to the background.
        fill(Rect{row.x, row.y, std::max(2.0f, m.hairline * 2.0f), row.h}, p.accent);
    } else if (hovered && !disabled) {
        fillRound(row, p.hover, radius);
    }
    const Rect textBox =
        row.inset(tokens::spacePx(m, Space::Md), 0.0f, tokens::spacePx(m, Space::Sm), 0.0f);
    textAt(m, textBox, label, disabled ? p.textDim : p.text, Type::Body, false, 0.0f, 0.5f);
    return clicked && !disabled;
}

bool listRowDoubleClicked() {
    return g_rowDouble;
}

bool listRowAt(const Metrics& m, const Rect& box, const std::string& label, bool selected,
               bool disabled, Type tier) {
    if (box.empty()) return false;
    const Palette p = palette();
    ImGui::SetCursorScreenPos(v2(box));
    // A row is a click surface, not a button: the keyboard-navigation cursor (a ring
    // in ImGuiCol_NavCursor, which the theme sets to the streaming accent) would be
    // painted around whichever row the nav system landed on first -- a red frame in
    // the middle of a panel. The selection fill already says where you are.
    ImGui::PushStyleColor(ImGuiCol_NavCursor, IM_COL32(0, 0, 0, 0));
    pushWidgetId(label, box);
    const bool clicked = ImGui::InvisibleButton("##row", ImVec2(box.w, box.h));
    const bool hovered = ImGui::IsItemHovered();
    g_rowDouble = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    popWidgetId();
    ImGui::PopStyleColor();

    const float radius = tokens::radiusPx(m, Radius::Xs);
    if (selected) {
        fillRound(box, p.selected, radius);
        fill(Rect{box.x, box.y, std::max(2.0f, m.hairline * 2.0f), box.h}, p.accent);
    } else if (hovered && !disabled) {
        fillRound(box, p.hover, radius);
    }
    const Rect textBox =
        box.inset(tokens::spacePx(m, Space::Sm), 0.0f, tokens::spacePx(m, Space::Xs), 0.0f);
    // A label starting with "##" is id-only, ImGui's own convention: the panels pass
    // a row's path there (unique and stable) and draw the row's text themselves.
    const bool drawLabel = !(label.size() >= 2 && label[0] == '#' && label[1] == '#');
    if (drawLabel) textAt(m, textBox, label, disabled ? p.textDim : p.text, tier, false, 0.0f, 0.5f);
    return clicked && !disabled;
}
void badge(const Metrics& m, const Rect& box, const std::string& label, ImU32 fg, ImU32 bg) {
    if (box.empty()) return;
    fillRound(box, bg, tokens::radiusPx(m, Radius::Pill));
    textAt(m, box, label, fg, Type::Caption, false, 0.5f, 0.5f);
}

// ---- scrolling ------------------------------------------------------------

bool beginScroll(const char* id, const Rect& box, const Metrics& m, ImU32 bg) {
    const Palette p = palette();
    ImGui::SetCursorScreenPos(v2(box));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, bg);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, p.scrollTrack);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, p.scrollThumb);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, p.accent);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, p.accentPressed);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, m.scrollbar);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, m.scrollbar * 0.5f);
    return ImGui::BeginChild(id, ImVec2(box.w, box.h), ImGuiChildFlags_None);
}

void scrollBy(float pixels) {
    ImGui::SetScrollY(ImGui::GetScrollY() + pixels);
}

float scrollMax() {
    return ImGui::GetScrollMaxY();
}

float scrollFromBottom() {
    return std::max(0.0f, ImGui::GetScrollMaxY() - ImGui::GetScrollY());
}

void scrollToBottom() { ImGui::SetScrollY(ImGui::GetScrollMaxY()); }

bool endScroll(bool follow) {
    // Careful: GetScrollMaxY() here is the PREVIOUS frame's content height
    // (imgui.cpp: "Update contents size from last frame"), so a jump issued from
    // this call lands one item short of a bubble that was just appended. The chat
    // no longer uses this path -- it pins with SetScrollHereY(1.0f) instead.
    if (follow) ImGui::SetScrollY(ImGui::GetScrollMaxY());
    const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2.0f;
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(5);
    return atBottom;
}

} // namespace ui
} // namespace studio
