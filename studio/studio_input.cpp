// proJV studio -- the prompt composer (see studio_input.h).
//
// ONE surface, drawn by ImGui itself. There is no card behind the input: the input
// frame IS the box. No child card, no extra stroke, and the keyboard-navigation
// cursor is pushed transparent, because that was the second ring (ImGui paints it
// around whatever item has focus, and the item is inset by its own padding, so the
// two outlines could never line up).
//
// It is a MULTILINE field, and that is a behaviour change, not a style one: the
// single-line field could not wrap and could not hold a second line at all. The
// keys follow the TUI's meaning and the composer's shape:
//
//   Enter            send (or fill the "/" menu)  -- the TUI's Enter
//   Shift+Enter      new line                     -- the one key the TUI never had
//   Ctrl+Enter       new line                     -- same, for IME users
//   Esc              clear the line / cancel / quit -- the TUI's three meanings
//
// The height is NOT decided here: the frame measured the text before it drew
// anything (Frame::composerRows) and gave the band that many rows, so the chat
// column shrinks by exactly what the prompt grows.
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
    // The arrow keys belong to the caret once the prompt holds more than one row;
    // the key map reads this before the widget is submitted.
    app.setPromptRows(f.composerRows);

    const layout::Rect box = f.composer;
    const float radius = tokens::radiusPx(m, Radius::Md);
    const float padX = tokens::spacePx(m, Space::Md);
    const float padY = tokens::spacePx(m, Space::Sm);

    const bool modalOpen = app.modal() != "none";

    // Focus policy, and the reason the header buttons were dead: a field that calls
    // SetKeyboardFocusHere() every frame keeps ImGui's active id forever, and while
    // an item owns the active id no other widget can be pressed. Ask once; take the
    // keyboard back only when the user clicks a spot nothing claimed, returns from a
    // dialog, or has just sent a line.
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

    // Enter validates, Shift+Enter and Ctrl+Enter insert a newline. That is
    // ImGuiInputTextFlags_CtrlEnterForNewLine's exact definition in multiline mode
    // (imgui_widgets.cpp: "Determine if we turn Enter into a \n character"), and
    // WordWrap is what makes a long line wrap instead of scrolling sideways.
    const ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue |
                                      ImGuiInputTextFlags_CtrlEnterForNewLine |
                                      ImGuiInputTextFlags_WordWrap |
                                      ImGuiInputTextFlags_EscapeClearsAll;
    if (!modalOpen && wantFocus) ImGui::SetKeyboardFocusHere();
    ui::pushType(m, Type::Mono, true);
    const bool entered = ImGui::InputTextMultiline("##prompt", g_buf, sizeof g_buf,
                                                  ImVec2(box.w, box.h), flags);
    const bool edited = ImGui::IsItemEdited();
    const bool active = ImGui::IsItemActive();
    ui::popType();
    if (active) wantFocus = false;

    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(7);

    // A multiline field has no hint mode of its own, so the placeholder is drawn
    // over the empty box. Not while it has the keyboard: there the caret marks the
    // spot, and grey text under a caret reads as typed text.
    if (g_buf[0] == '\0' && !active && !s.input.placeholder.empty()) {
        const float rowH = tokens::typePx(m, Type::Mono);
        ui::textAt(m,
                   layout::Rect{box.x + padX + m.hairline, box.y + padY + m.hairline,
                                std::max(1.0f, box.w - 2.0f * padX), rowH},
                   s.input.placeholder, p.textDim, Type::Mono, true, 0.0f, 0.0f);
    }

    if (edited) app.commitInput(g_buf);
    if (entered) {
        // The "/" menu owns Enter while it is open: it fills the line, never sends.
        if (app.slashOpen())
            app.slashFill();
        else
            app.send();
        // Enter deactivates the field (that is what "validated" means in ImGui); a
        // composer that needs a click before the next line is a composer that gets
        // used once.
        wantFocus = true;
    }
}

} // namespace studio
