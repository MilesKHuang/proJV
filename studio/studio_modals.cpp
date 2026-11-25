// proJV studio -- the ten modals, on one dialog shell (design 08.5, B5).
//
// One modal at a time, like the TUI's nested Modal() stack: while one is open the
// rest of the screen sits behind a dimmer and gets no input. The rows come from
// the snapshot (view_model::Screen), so what a dialog says and what a snapshot of
// it says cannot disagree.
//
// What the rebuild changed: there is ONE dialog shell (title bar with a close
// affordance, a scrolling body, a right-aligned action footer) and every body is
// built from studio/ui, so ten dialogs finally look like ten pages of the same
// application instead of ten applications. Sizes come from the token ladder: the
// old per-dialog pixel table clipped its own text at 150% DPI.
#include "studio_ui.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_stdlib.h"

#include "design/fit.h"
#include "design/layout.h"
#include "design/tokens.h"
#include "studio_app.h"
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

ImU32 withAlpha(ImU32 c, float a) {
    const int alpha = static_cast<int>(std::lround(std::min(1.0f, std::max(0.0f, a)) * 255.0f));
    return (c & ~IM_COL32_A_MASK) | (static_cast<ImU32>(alpha) << IM_COL32_A_SHIFT);
}

// The dialog's three bands, computed from the window and the token ladder.
struct DialogBox {
    Rect box;
    Rect header;
    Rect body;
    Rect footer;
};

DialogBox computeDialog(const Metrics& m, float bodyRows) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float pad = tokens::spacePx(m, Space::Md);
    const float maxW = std::min(860.0f * m.factor, vp->WorkSize.x - 2.0f * m.pagePad);
    const float minW = std::min(420.0f * m.factor, maxW);
    const float w = std::min(std::max(vp->WorkSize.x * 0.52f, minW), maxW);
    const float maxH = vp->WorkSize.y - 2.0f * m.pagePad;

    // The height follows the content: a five-row help page is a small dialog, a
    // twenty-row config page scrolls. A fixed fraction of the window made every
    // short dialog look like an empty form.
    const float headerH = std::max(tokens::linePx(m, Type::Heading), m.controlH);
    const float footerH = m.controlH;
    const float chrome = 2.0f * pad + headerH + footerH + 2.0f * tokens::spacePx(m, Space::Sm);
    const float wantedBody =
        std::max(1.0f, bodyRows) * m.rowH + 2.0f * tokens::spacePx(m, Space::Sm);
    const float h = std::min(maxH, std::max(chrome + 96.0f * m.factor, chrome + wantedBody));

    DialogBox d;
    d.box = Rect{vp->WorkPos.x + (vp->WorkSize.x - w) * 0.5f,
                 vp->WorkPos.y + (vp->WorkSize.y - h) * 0.5f, w, h};
    const Rect inner = d.box.inset(pad, pad, pad, pad);
    // One title height and one footer height for all ten: that is the whole point
    // of a shell.
    d.header = Rect{inner.x, inner.y, inner.w, headerH};
    d.footer = Rect{inner.x, inner.bottom() - footerH, inner.w, footerH};
    const float bodyTop = d.header.bottom() + tokens::spacePx(m, Space::Sm);
    d.body = Rect{inner.x, bodyTop, inner.w,
                  std::max(1.0f, d.footer.y - tokens::spacePx(m, Space::Sm) - bodyTop)};
    return d;
}

std::string titleFor(const view_model::Screen& s, const std::string& modal) {
    if (modal == "approval") return s.approval.title;
    if (modal == "config") return s.config.title;
    if (modal == "mcp") return "MCP Servers";
    if (modal == "open") return s.open.title;
    if (modal == "model") return s.modelList.title;
    if (modal == "theme") return s.themeList.title;
    if (modal == "theme_editor") return s.themeEditor.title;
    if (modal == "help") return s.help.title;
    if (modal == "quit") return s.quit.title;
    if (modal == "welcome") return s.welcome.title;
    return "proJV";
}

// Roughly how many rows this dialog's body holds, so the shell can size itself
// to its content instead of guessing a fraction of the window.
float bodyRows(const std::string& modal, const view_model::Screen& s) {
    if (modal == "help") return static_cast<float>(s.help.rows.size()) + 2.0f;
    if (modal == "mcp") return static_cast<float>(s.mcp.rows.size()) + 3.0f;
    if (modal == "open") return static_cast<float>(s.open.rows.size()) + 2.0f;
    if (modal == "model") return static_cast<float>(s.modelList.rows.size()) + 2.0f;
    if (modal == "theme") return static_cast<float>(s.themeList.rows.size()) + 2.0f;
    if (modal == "approval")
        return 5.0f + static_cast<float>(s.approval.targets.size()) +
               static_cast<float>(s.approval.moreTargets > 0 ? 1 : 0);
    if (modal == "quit") return 3.0f;
    if (modal == "welcome") return 5.0f;
    if (modal == "config") return static_cast<float>(s.config.fields.size()) * 1.8f + 3.0f;
    if (modal == "theme_editor") return 14.0f;
    return 8.0f;
}
// ---- bodies --------------------------------------------------------------
// Each body draws inside the body scroll area and advances the cursor; the width
// it is given already excludes the scrollbar, so nothing is clipped by it.

void bodyApproval(App& app, const Metrics& m, const Palette& p) {
    const view_model::ApprovalPanel& a = app.screen().approval;
    ui::paragraph(m, a.intro, p.text, Type::Body);
    ImGui::Dummy(ImVec2(1.0f, tokens::spacePx(m, Space::Xs)));

    // The command lives in its own surface: it is the thing being approved.
    const float cmdW = ImGui::GetContentRegionAvail().x;
    const float lineH = tokens::linePx(m, Type::Mono);
    const int lines = std::max(
        1,
        static_cast<int>(ImGui::CalcTextSize(a.command.c_str(), nullptr, false, cmdW).y / lineH));
    const Rect cmdBox{ImGui::GetCursorScreenPos().x, ImGui::GetCursorScreenPos().y, cmdW,
                      lineH * static_cast<float>(std::min(lines, 6)) +
                          2.0f * tokens::spacePx(m, Space::Sm)};
    ui::fillRound(cmdBox, p.inputBg, tokens::radiusPx(m, Radius::Sm));
    ui::stroke(cmdBox, p.border, m.hairline, tokens::radiusPx(m, Radius::Sm));
    ImGui::SetCursorScreenPos(
        ImVec2(cmdBox.x + tokens::spacePx(m, Space::Sm), cmdBox.y + tokens::spacePx(m, Space::Sm)));
    ImGui::PushTextWrapPos(cmdBox.right() - tokens::spacePx(m, Space::Sm));
    ui::text(m, a.command, studio_theme::field(&ThemeColors::phaseAwaitApproval), Type::Mono, true);
    ImGui::PopTextWrapPos();
    ImGui::SetCursorScreenPos(ImVec2(cmdBox.x, cmdBox.bottom() + tokens::spacePx(m, Space::Sm)));
    ImGui::Dummy(ImVec2(1.0f, 1.0f));

    if (!a.targets.empty()) {
        ui::text(m, "Targets", p.textDim, Type::Small);
        for (const auto& t : a.targets)
            ui::text(m, "  - " + t, studio_theme::field(&ThemeColors::phaseAwaitApproval),
                     Type::Small);
        if (a.moreTargets > 0) ui::hint(m, "  ... and " + std::to_string(a.moreTargets) + " more");
    }
    ui::divider(m);
    ui::hint(m, a.explain);
    ui::hint(m, a.undo);
}

void bodyConfig(App& app, const Metrics& m, const Palette& p) {
    const view_model::ConfigPanel& c = app.screen().config;
    ConfigForm& form = app.configForm();
    const float width = ImGui::GetContentRegionAvail().x;
    const float labelW = std::min(210.0f * m.factor, width * 0.42f);
    const float gap = tokens::spacePx(m, Space::Md);

    for (int i = 0; i < static_cast<int>(c.fields.size()); ++i) {
        const view_model::ConfigField& f = c.fields[static_cast<size_t>(i)];
        ImGui::PushID(i);
        if (f.kind == "radio") {
            ui::text(m, f.label, p.text, Type::Body);
            for (int k = 0; k < static_cast<int>(f.options.size()); ++k) {
                const bool sel =
                    (f.label == "Effort") ? form.effortIndex == k : form.modeIndex == k;
                if (ui::radio(m, f.options[static_cast<size_t>(k)], sel)) {
                    if (f.label == "Effort")
                        form.effortIndex = k;
                    else
                        form.modeIndex = k;
                }
            }
            if (f.label == "Command approval" &&
                ui::button(m, "Clear remembered commands", ui::Btn::Secondary))
                app.clearGrants();
        } else if (f.kind == "note") {
            ui::hint(m, f.label + ": " + f.value);
        } else {
            // Two columns: a fixed label gutter and the control beside it. The
            // gutter scales with the tokens, so a 150% monitor no longer clips
            // "C++ Compiler" into "C++ Compile".
            const float y = ImGui::GetCursorScreenPos().y;
            const float rowH = (f.kind == "check") ? m.rowH : m.controlH;
            Rect labelBox{ImGui::GetCursorScreenPos().x, y, labelW - gap, rowH};
            Rect controlBox{labelBox.right() + gap, y, width - labelW, rowH};
            if (f.kind != "check") {
                ui::textAt(m, labelBox, ui::fitLine(m, f.label, labelBox.w, Type::Body, false),
                           p.text, Type::Body, false, 0.0f);
            }
            ImGui::SetCursorScreenPos(ImVec2(controlBox.x, controlBox.y));
            if (f.kind == "check") {
                bool checked = form.osc;
                if (ui::checkbox(m, f.label, &checked)) form.osc = checked;
            } else {
                std::string* buf = app.configBufferAt(i);
                if (buf) {
                    const bool entered =
                        ui::field(m, "##v", buf, nullptr, f.kind == "password", controlBox.w);
                    (void)entered;
                }
            }
            ImGui::SetCursorScreenPos(ImVec2(labelBox.x, y + rowH + tokens::spacePx(m, Space::Xs)));
            ImGui::Dummy(ImVec2(1.0f, 1.0f));
        }
        ImGui::PopID();
    }
    ui::divider(m);
    ui::hint(m, c.hint);
}

void bodyMcp(App& app, const Metrics& m, const Palette& p) {
    const view_model::McpPanel& mp = app.screen().mcp;
    if (mp.rows.empty()) {
        ui::hint(m, "(no servers in projv_files/mcp.json)");
    } else {
        for (int i = 0; i < static_cast<int>(mp.rows.size()); ++i) {
            const view_model::McpRow& r = mp.rows[static_cast<size_t>(i)];
            std::string line =
                std::string(r.enabled ? "[on]  " : "[off] ") + r.name + "   " + r.state;
            if (r.tools > 0) line += "   " + std::to_string(r.tools) + " tools";
            if (!r.trusted) line += "   (needs \"trusted\": true)";
            if (ui::listRow(m, line, app.listSelected() == i, !r.trusted)) app.listSelected() = i;
            if (ui::listRowDoubleClicked()) app.listActivate();
        }
    }
    if (!mp.purpose.empty()) ui::hint(m, mp.purpose);
    if (!mp.error.empty()) ui::paragraph(m, mp.error, p.danger, Type::Small);
    if (ImGui::IsKeyPressed(ImGuiKey_R, false)) {
        const int sel = app.listSelected();
        if (sel >= 0 && sel < static_cast<int>(mp.rows.size()))
            app.restartMcp(mp.rows[static_cast<size_t>(sel)].name);
    }
}

void bodyList(App& app, const Metrics& m, const view_model::ListPanel& panel) {
    if (panel.rows.empty()) {
        ui::hint(m, "(empty)");
        return;
    }
    for (int i = 0; i < static_cast<int>(panel.rows.size()); ++i) {
        if (ui::listRow(m, panel.rows[static_cast<size_t>(i)], app.listSelected() == i))
            app.listSelected() = i;
        if (ui::listRowDoubleClicked()) app.listActivate();
    }
}

void bodyThemeEditor(App& app, const Metrics& m, const Palette& p) {
    const view_model::ThemeEditorPanel& te = app.screen().themeEditor;
    const float width = ImGui::GetContentRegionAvail().x;
    const float listW = std::min(220.0f * m.factor, width * 0.42f);
    const float height = std::max(80.0f, ImGui::GetContentRegionAvail().y);
    const ImVec2 at = ImGui::GetCursorScreenPos();

    const Rect listBox{at.x, at.y, listW, height};
    ui::fillRound(listBox, p.inputBg, tokens::radiusPx(m, Radius::Sm));
    const bool listOpen =
        ui::beginScroll("##tfields", listBox.inset(tokens::spacePx(m, Space::Xs)), m, p.inputBg);
    if (listOpen) {
        for (int i = 0; i < static_cast<int>(te.fields.size()); ++i) {
            if (ui::listRow(m, te.fields[static_cast<size_t>(i)], app.themeField() == i))
                app.themeSelect(i);
        }
    }
    ui::endScroll(false);

    const Rect editBox{listBox.right() + tokens::spacePx(m, Space::Lg), at.y,
                       width - listW - tokens::spacePx(m, Space::Lg), height};
    ImGui::SetCursorScreenPos(ImVec2(editBox.x, editBox.y));
    ui::text(m, te.field, p.text, Type::Heading);
    ImGui::Dummy(ImVec2(1.0f, tokens::spacePx(m, Space::Sm)));
    ui::text(m, "Hex", p.textDim, Type::Small);
    ImGui::SetCursorScreenPos(ImVec2(editBox.x, ImGui::GetCursorScreenPos().y));
    ui::field(m, "##hex", &app.themeHex(), "#RRGGBB", false,
              std::min(160.0f * m.factor, editBox.w));
    ImGui::Dummy(ImVec2(1.0f, tokens::spacePx(m, Space::Sm)));
    const Rect swatch{editBox.x, ImGui::GetCursorScreenPos().y,
                      std::min(160.0f * m.factor, editBox.w), 36.0f * m.factor};
    ui::fillRound(swatch, studio_theme::fromHex(app.themeHex()), tokens::radiusPx(m, Radius::Sm));
    ui::stroke(swatch, p.border, m.hairline, tokens::radiusPx(m, Radius::Sm));
    ImGui::SetCursorScreenPos(ImVec2(editBox.x, swatch.bottom() + tokens::spacePx(m, Space::Sm)));
    ui::hint(m, te.hint);
    // Keep the body's own cursor below both columns so nothing else overlaps.
    ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + height));
    ImGui::Dummy(ImVec2(1.0f, 1.0f));
}

void bodyHelp(App& app, const Metrics& m, const Palette& p) {
    const view_model::HelpPanel& h = app.screen().help;
    const float keyW = std::min(96.0f * m.factor, ImGui::GetContentRegionAvail().x * 0.3f);
    for (const auto& row : h.rows) {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ui::textAt(m, Rect{at.x, at.y, keyW, m.rowH}, row.key,
                   studio_theme::field(&ThemeColors::phaseExecutingTools), Type::Small, true, 0.0f);
        ui::textAt(m, Rect{at.x + keyW, at.y, ImGui::GetContentRegionAvail().x - keyW, m.rowH},
                   row.desc, p.text, Type::Small, false, 0.0f);
        ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + m.rowH));
    }
    ui::divider(m);
    ui::hint(m, h.hint);
}

void bodyQuit(App& app, const Metrics& m, const Palette& p) {
    const view_model::QuitPanel& q = app.screen().quit;
    ui::paragraph(m, q.body, p.text, Type::Body);
}

void bodyWelcome(App& app, const Metrics& m, const Palette& p) {
    const view_model::WelcomePanel& w = app.screen().welcome;
    ui::paragraph(m, w.body, p.text, Type::Body);
    ImGui::Dummy(ImVec2(1.0f, tokens::spacePx(m, Space::Sm)));
    ui::text(m, w.keyLabel, p.text, Type::Small);
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, ImGui::GetCursorScreenPos().y));
    // The first thing a new user must do is type a key here, so the field takes
    // focus when the dialog appears. Enter is handled by the FIELD (EnterReturnsTrue)
    // rather than by a global IsKeyPressed check, which the focused field swallows.
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    const bool entered = ui::field(m, "##apikey", &app.configForm().apiKey, "sk-...", true,
                                   std::min(420.0f * m.factor, ImGui::GetContentRegionAvail().x));
    if (entered) app.saveApiKey();
    ImGui::Dummy(ImVec2(1.0f, tokens::spacePx(m, Space::Xs)));
    ui::hint(m, w.hint);
}

// ---- footer --------------------------------------------------------------

// One action in a dialog footer. `id` is what the shell dispatches on, so a body
// never has to know where its buttons are.
enum class Act {
    None,
    Approve,
    ApproveAlways,
    Deny,
    SaveConfig,
    Close,
    Ok,
    RestartMcp,
    ThemeApply,
    ThemeSave,
    SaveKey,
    Quit
};

struct Action {
    std::string label;
    ui::Btn kind;
    Act act;
};

std::vector<Action> actionsFor(const std::string& modal, const view_model::Screen& s) {
    std::vector<Action> out;
    if (modal == "approval") {
        const auto& b = s.approval.buttons;
        if (b.size() >= 3) {
            out.push_back({b[2], ui::Btn::Ghost, Act::Deny});
            if (s.approval.showAlways)
                out.push_back({b[1], ui::Btn::Secondary, Act::ApproveAlways});
            out.push_back({b[0], ui::Btn::Primary, Act::Approve});
        }
    } else if (modal == "config") {
        const auto& b = s.config.buttons;
        if (b.size() >= 2) {
            out.push_back({b[1], ui::Btn::Secondary, Act::Close});
            out.push_back({b[0], ui::Btn::Primary, Act::SaveConfig});
        }
    } else if (modal == "mcp") {
        const auto& b = s.mcp.buttons;
        if (b.size() >= 2) {
            out.push_back({b[0], ui::Btn::Secondary, Act::RestartMcp});
            out.push_back({b[1], ui::Btn::Primary, Act::Close});
        }
    } else if (modal == "open" || modal == "model" || modal == "theme") {
        out.push_back({"Close", ui::Btn::Secondary, Act::Close});
        out.push_back({"Open", ui::Btn::Primary, Act::Ok});
    } else if (modal == "theme_editor") {
        const auto& b = s.themeEditor.buttons;
        if (b.size() >= 3) {
            out.push_back({b[2], ui::Btn::Ghost, Act::Close});
            out.push_back({b[1], ui::Btn::Secondary, Act::ThemeSave});
            out.push_back({b[0], ui::Btn::Primary, Act::ThemeApply});
        }
    } else if (modal == "help") {
        const auto& b = s.help.buttons;
        if (!b.empty()) out.push_back({b[0], ui::Btn::Primary, Act::Close});
    } else if (modal == "quit") {
        const auto& b = s.quit.buttons;
        if (b.size() >= 2) {
            out.push_back({b[0], ui::Btn::Secondary, Act::Close});
            out.push_back({b[1], ui::Btn::Danger, Act::Quit});
        }
    } else if (modal == "welcome") {
        out.push_back({s.welcome.button, ui::Btn::Primary, Act::SaveKey});
    }
    return out;
}

void dispatch(App& app, const std::string& modal, Act act) {
    switch (act) {
    case Act::Approve:
        app.approveTool(1);
        break;
    case Act::ApproveAlways:
        app.approveTool(2);
        break;
    case Act::Deny:
        app.approveTool(0);
        break;
    case Act::SaveConfig:
        app.saveConfigFromForm();
        break;
    case Act::Close:
        app.closeModal();
        break;
    case Act::Ok:
        app.listActivate();
        break;
    case Act::RestartMcp: {
        const auto& rows = app.screen().mcp.rows;
        const int sel = app.listSelected();
        if (sel >= 0 && sel < static_cast<int>(rows.size()))
            app.restartMcp(rows[static_cast<size_t>(sel)].name);
        break;
    }
    case Act::ThemeApply:
        app.themeApply();
        break;
    case Act::ThemeSave:
        app.themeSave();
        break;
    case Act::SaveKey:
        app.saveApiKey();
        break;
    case Act::Quit:
        app.requestExit();
        break;
    case Act::None:
    default:
        break;
    }
    (void)modal;
}

// What Enter does in each dialog. The approval dialog's primary action is YES, so
// Enter approves; every other dialog treats Enter as "the one button I came here
// for", which is the TUI's initial-focus rule.
Act enterAction(const std::string& modal) {
    if (modal == "approval") return Act::Approve;
    if (modal == "help" || modal == "quit") return Act::Close;
    if (modal == "open" || modal == "model" || modal == "theme") return Act::Ok;
    return Act::None;
}

void drawFooter(App& app, const std::string& modal, const Metrics& m, const Rect& box) {
    const std::vector<Action> actions = actionsFor(modal, app.screen());
    const float gap = tokens::spacePx(m, Space::Sm);
    float right = box.right();
    for (auto it = actions.rbegin(); it != actions.rend(); ++it) {
        const float w = ui::buttonWidth(m, it->label, it->kind);
        const Rect btn{right - w, box.y, w, box.h};
        if (ui::buttonAt(m, btn, it->label, it->kind)) dispatch(app, modal, it->act);
        right -= w + gap;
    }
    // A hint on the left of the footer, clipped to what the buttons left over.
    std::string hint;
    if (modal == "approval")
        hint = "Enter approves,  Esc is not an answer";
    else if (modal == "config")
        hint = "Esc or F2 closes";
    else if (modal == "help")
        hint = "Enter closes";
    else if (modal == "mcp")
        hint = "Enter toggles,  R restarts";
    else if (modal == "quit")
        hint = "Ctrl+C never quits";
    if (!hint.empty() && right > box.x) {
        ui::textAt(m, Rect{box.x, box.y, right - box.x - gap, box.h},
                   ui::fitLine(m, hint, right - box.x - gap, Type::Small, false),
                   ui::palette().textDim, Type::Small, false, 0.0f);
    }
}

} // namespace

void drawModals(App& app) {
    const std::string modal = app.modal();
    if (modal == "none") return;
    const Metrics m = ui::metricsFor(app.dpiScale());
    const Palette p = ui::palette();
    const view_model::Screen& s = app.screen();
    const ImGuiViewport* vp = ImGui::GetMainViewport();

    // A full-viewport blocker, not an ImGui popup: it is stateless (no popup stack
    // to leak when the agent closes the approval dialog on its own) and it still
    // takes every click, so the screen behind a modal is read-only.
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, withAlpha(p.window, 0.66f));
    // NOTE: no ImGuiWindowFlags_NoBringToFrontOnFocus here. With that flag a freshly
    // created window is NOT raised, so this blocker was painted UNDER the main
    // window: the dialog existed and was completely invisible, which made the whole
    // app look dead because an unseen modal swallowed every key.
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                   ImGuiWindowFlags_NoScrollWithMouse |
                                   ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("##modalblocker", nullptr, flags)) {
        const DialogBox d = computeDialog(m, bodyRows(modal, s));
        const float radius = tokens::radiusPx(m, Radius::Lg);
        ui::fillRound(d.box, p.overlay, radius);
        ui::stroke(d.box, p.border, m.hairline, radius);

        // Title, then the close affordance: one header for all ten dialogs.
        const float closeW = m.controlH;
        const bool closable = modal != "approval" && modal != "welcome";
        Rect titleBox = d.header;
        if (closable) titleBox.w -= closeW + tokens::spacePx(m, Space::Sm);
        const Type titleTiers[2] = {Type::Title, Type::Heading};
        ui::fittedText(m, titleBox, titleFor(s, modal), p.text, titleTiers, 2, false, 0.0f);
        if (closable) {
            const Rect closeBox{d.header.right() - closeW, d.header.y, closeW, m.controlH};
            if (ui::iconButton(m, closeBox, "x")) app.closeModal();
        }
        ui::fill(Rect{d.box.x + tokens::spacePx(m, Space::Md),
                      d.header.bottom() + tokens::spacePx(m, Space::Xs) * 0.5f,
                      d.box.w - 2.0f * tokens::spacePx(m, Space::Md), m.hairline},
                 p.divider);

        // The body scrolls: a dialog whose content does not fit must never push its
        // own footer off the bottom of the screen.
        const bool open = ui::beginScroll("##dlgbody", d.body, m, p.overlay);
        if (open) {
            if (modal == "approval")
                bodyApproval(app, m, p);
            else if (modal == "config")
                bodyConfig(app, m, p);
            else if (modal == "mcp")
                bodyMcp(app, m, p);
            else if (modal == "open")
                bodyList(app, m, s.open);
            else if (modal == "model")
                bodyList(app, m, s.modelList);
            else if (modal == "theme")
                bodyList(app, m, s.themeList);
            else if (modal == "theme_editor")
                bodyThemeEditor(app, m, p);
            else if (modal == "help")
                bodyHelp(app, m, p);
            else if (modal == "quit")
                bodyQuit(app, m, p);
            else if (modal == "welcome")
                bodyWelcome(app, m, p);
        }
        ui::endScroll(false);

        drawFooter(app, modal, m, d.footer);

        // Enter is the primary action, exactly as the TUI's initial focus is. The
        // text fields in this dialog consume Enter themselves (EnterReturnsTrue),
        // so this only fires when nothing has the keyboard.
        if (!ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
            switch (enterAction(modal)) {
            case Act::Approve:
                app.approveTool(1);
                break;
            case Act::Close:
                app.closeModal();
                break;
            case Act::Ok:
                app.listActivate();
                break;
            default:
                break;
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

} // namespace studio
