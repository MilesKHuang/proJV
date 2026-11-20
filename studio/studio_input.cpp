// proJV studio -- the prompt composer (see studio_input.h).
//
// ONE surface, drawn by ImGui itself. There is no card behind the input: the input
// frame IS the box. Everything that used to make it look like two rectangles is
// gone -- no child card, no extra stroke, and the keyboard-navigation cursor is
// pushed transparent, because that was the second ring (ImGui paints it around
// whatever item has focus, and the item is inset by its own padding, so the two
// outlines could never line up).
//
// Behaviour is unchanged: the app owns the text, Enter sends (or fills the "/"
// menu), Esc clears, and the field asks for the keyboard once instead of every
// frame -- re-grabbing focus each frame made every other control unclickable.
#include "studio_input.h"

#include <algorithm>
#include <cstdio>
#include <string>

#include "imgui.h"

#include "design/tokens.h"
#include "studio_app.h"
#include "studio_theme.h"
#include "studio_ui.h"
#include "tui/theme_manager.h"
#include "ui/ui.h"

namespace studio {

using tokens::Radius;
using tokens::Space;
using tokens::Type;

namespace {

char g_buf[8192];

} // namespace

void drawPrompt(App& app, const Frame& f) {
    const view_model::Screen& s = app.screen();
    const tokens::Metrics& m = f.metrics;
    const ui::Palette p = ui::palette();

    // The app is the truth. Anything that edited the line outside this widget (a
    // slash fill, Esc, Enter, the paste cap) shows up here as a difference and is
    // copied in. No flag to get out of sync.
    if (app.inputText() != g_buf) std::snprintf(g_buf, sizeof g_buf, "%s", app.inputText().c_str());

    const layout::Rect box = f.composer;
    const float radius = tokens::radiusPx(m, Radius::Md);
    const float lineH = tokens::linePx(m, Type::Mono);
    const float padX = tokens::spacePx(m, Space::Md);
    const float padY = std::max(0.0f, (box.h - lineH) * 0.5f - m.hairline);

    const bool modalOpen = app.modal() != "none";

    // Focus policy, and the reason the header buttons were dead: a field that calls
    // SetKeyboardFocusHere() every frame keeps ImGui's active id forever, and while
    // an item owns the active id no other widget can be pressed. Ask once; take the
    // keyboard back only when the user clicks a spot nothing claimed, or returns
    // from a dialog.
    static bool wantFocus = true;
    static bool wasModal = false;
    if (wasModal && !modalOpen) wantFocus = true;
    wasModal = modalOpen;
    if (!modalOpen && !wantFocus && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        !ImGui::IsAnyItemActive())
        wantFocus = true;

    ImGui::SetCursorScreenPos(ImVec2(box.x, box.y));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, p.surface);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, p.surface);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, p.surface);
    ImGui::PushStyleColor(ImGuiCol_Text, p.text);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, p.textDim);
    ImGui::PushStyleColor(ImGuiCol_Border, p.border);
    // Transparent: ImGui's nav/focus highlight is the "second border" that never
    // lined up with the first one. The caret already shows where typing goes.
    ImGui::PushStyleColor(ImGuiCol_NavCursor, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, radius);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, m.hairline);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(padX, padY));

    if (!modalOpen && wantFocus) ImGui::SetKeyboardFocusHere();
    const ImGuiInputTextFlags flags =
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_EscapeClearsAll;
    ui::pushType(m, Type::Mono, true);
    ImGui::SetNextItemWidth(box.w);
    const bool entered = ImGui::InputTextWithHint("##prompt", s.input.placeholder.c_str(), g_buf,
                                                 sizeof g_buf, flags);
    const bool edited = ImGui::IsItemEdited();
    if (ImGui::IsItemActive()) wantFocus = false;
    ui::popType();

    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(7);

    if (edited) app.commitInput(g_buf);
    if (entered) {
        // The "/" menu owns Enter while it is open: it fills the line, never sends.
        if (app.slashOpen())
            app.slashFill();
        else
            app.send();
    }
}

} // namespace studio
