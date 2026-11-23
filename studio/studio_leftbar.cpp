// proJV studio -- the left column: sessions, the file tree.
//
// One switchboard, two panels stacked in one column (the shape of the reference
// window this was designed from):
//
//   Sessions   the panes THIS window owns, each one CARD: title + state on the first
//              line, then CPU / MEM / GPU as three thin progress bars.
//   Files      the workspace tree, with `>`/`@` on every row: send the path, or send
//              `@path` (the context form proJV already understands).
//
// This file draws and wires; it owns no state. Two rules learned the hard way:
//   - every row is reserved with a Dummy() before it is drawn at absolute rects, or
//     ImGui reports "SetCursorScreenPos() to extend window boundaries" and paints the
//     panel in pure red;
//   - a context menu is opened by the row and DRAWN ONCE after the loop: the same
//     popup name queried once per row is an ImGui id conflict.
#include "studio_leftbar.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "imgui.h"

#include "design/gauges.h"
#include "design/path_text.h"
#include "design/tokens.h"
#include "platform/isystem_stats.h"
#include "studio_app.h"
#include "studio_theme.h"
#include "studio_ui.h"
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

ImU32 fld(const std::string ThemeColors::*field) { return studio_theme::field(field); }

struct Panel {
    Rect head;
    Rect body;
};

Panel splitPanel(const Metrics& m, const Rect& box) {
    Panel out;
    if (box.empty()) return out;
    const float pad = tokens::spacePx(m, Space::Sm);
    out.head = Rect{box.x + pad, box.y + pad, std::max(1.0f, box.w - 2.0f * pad), m.rowH};
    const float bodyY = out.head.bottom();
    out.body = Rect{box.x + pad, bodyY, std::max(1.0f, box.w - 2.0f * pad),
                    std::max(0.0f, box.bottom() - bodyY - pad)};
    return out;
}

void drawPanelTitle(const Metrics& m, const Rect& head, const std::string& title,
                    const std::string& right, const Palette& p) {
    if (head.empty()) return;
    ui::textAt(m, head, title, p.textDim, Type::Small, false, 0.0f, 0.5f);
    if (right.empty()) return;
    const float w = std::min(head.w, ui::measure(m, right, Type::Small, false));
    ui::textAt(m, Rect{head.right() - w, head.y, w, head.h},
               ui::fitLine(m, right, w, Type::Small, false), p.textDim, Type::Small, false, 0.0f,
               0.5f);
}

// One metric as a progress bar: a caption line ("CPU" left, the value right) and a
// slim track under it. The track has no outline -- at four pixels an outline IS the
// bar.
void metricBar(const Metrics& m, const Rect& cap, float barH, const std::string& label,
               const std::string& value, double fraction, ImU32 color,
               const std::string& tooltip) {
    if (cap.empty()) return;
    const Palette p = ui::palette();
    const Type tier = Type::Caption;
    ui::textAt(m, cap, label, p.textDim, tier, false, 0.0f, 0.5f);
    const float vw = std::min(cap.w * 0.6f, ui::measure(m, value, tier, false));
    ui::textAt(m, Rect{cap.right() - vw, cap.y, vw, cap.h},
               ui::fitLine(m, value, vw, tier, false), p.text, tier, false, 1.0f, 0.5f);
    const float h = std::max(4.0f, barH);
    const Rect track{cap.x, cap.bottom(), cap.w, h};
    const float radius = h * 0.5f;
    ui::fillRound(track, p.inputBg, radius);
    const double f = fraction < 0.0 ? 0.0 : (fraction > 1.0 ? 1.0 : fraction);
    if (f > 0.0) {
        Rect fillBox = track;
        fillBox.w = std::max(radius * 2.0f, track.w * static_cast<float>(f));
        ui::fillRound(fillBox, studio_theme::mix(p.inputBg, color, 0.65f), radius);
    }
    if (!tooltip.empty() && ImGui::IsMouseHoveringRect(ImVec2(track.x, track.y),
                                                      ImVec2(track.right(), track.bottom())) &&
        ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows))
        ImGui::SetTooltip("%s", tooltip.c_str());
}

std::string paneStateText(const view_model::Screen& s, bool busy) {
    if (busy && !s.statusText.empty()) return s.statusText;
    return s.phaseName.empty() ? std::string("Idle") : s.phaseName;
}

// One session as ONE card: a title line (name + state) and three progress bars
// inside a single surface with one border. The title and the gauges used to be two
// loose strips that happened to sit next to each other; the container is what makes
// the row read as one object.
void drawSessionRow(App& app, const Metrics& m, const Rect& row, int index) {
    const Palette p = ui::palette();
    const bool activeRow = index == app.activePane();
    const bool busy = app.paneBusy(index);
    const std::string state = paneStateText(app.screenOf(index), busy);
    const float gap = tokens::spacePx(m, Space::Xs);
    const float padX = tokens::spacePx(m, Space::Sm);
    const float radius = tokens::radiusPx(m, Radius::Md);
    const float lineH = m.rowH;

    // The whole card is one click target: click, right click and middle click all
    // belong to the pane. The bars submit no items, so they cannot steal the click.
    ImGui::SetCursorScreenPos(ImVec2(row.x, row.y));
    ImGui::PushID(index + 9000);
    const bool clicked = ImGui::InvisibleButton("##pane_card", ImVec2(row.w, row.h));
    const bool hovered = ImGui::IsItemHovered();
    const bool rightClicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);
    const bool middleClicked = ImGui::IsMouseClicked(ImGuiMouseButton_Middle);
    ImGui::PopID();

    if (activeRow) ui::fillRound(row, p.selected, radius);
    else if (hovered) ui::fillRound(row, p.hover, radius);
    ui::stroke(row, activeRow ? p.accent : p.border,
               activeRow ? std::max(1.0f, m.hairline * 1.5f) : m.hairline, radius);

    const Rect top{row.x + padX, row.y + gap, std::max(1.0f, row.w - 2.0f * padX), lineH};
    const float stateW = std::min(top.w * 0.45f, ui::measure(m, state, Type::Small, false));
    const Rect nameBox{top.x, top.y, std::max(20.0f, top.w - stateW), lineH};
    ui::textAt(m, nameBox, ui::fitLine(m, app.paneTitle(index), nameBox.w, Type::Small, false),
               activeRow ? p.text : p.textDim, Type::Small, false, 0.0f, 0.5f);
    ui::textAt(m, Rect{top.right() - stateW, top.y, stateW, lineH},
               ui::fitLine(m, state, stateW, Type::Small, false),
               busy ? fld(&ThemeColors::statusRunning) : p.textDim, Type::Small, false, 1.0f, 0.5f);

    // The figures, one progress bar per metric, stacked inside the same container. A
    // session with no child process costs nothing measurable: the sampler reports
    // valid zeros there, and an unmeasurable GPU prints "-" with the reason. A pane
    // that is not ready yet shows "-" everywhere -- "0% / 0 KB / 0%" on a card that is
    // still starting reads as "measured, idle", which is a different claim.
    const GroupSample sample = app.paneSample(index);
    const bool ready = app.paneReady(index);
    const float capH = tokens::linePx(m, Type::Caption);
    const float barH = std::max(4.0f, m.hairline * 4.0f);
    const float barW = std::max(1.0f, row.w - 2.0f * padX);
    const double cpuPct = sample.cpuValid ? sample.cpuPercent : 0.0;
    const std::string memValue =
        ready && sample.memValid ? gauges::humanBytes(sample.rssBytes) : std::string("-");
    std::optional<double> gpuPct;
    if (ready && sample.gpu.valid) gpuPct = sample.gpu.utilPercent;
    const ImU32 dim = p.textDim;
    float y = top.bottom() + gap;
    metricBar(m, Rect{row.x + padX, y, barW, capH}, barH, "CPU",
              ready ? gauges::percentLabel(sample.cpuValid ? cpuPct : 0.0) : std::string("-"),
              ready ? gauges::fraction(cpuPct, 100.0) : 0.0, ready ? p.accent : dim,
              ready ? "this session's processes, percent of the whole machine"
                    : "this session is still starting");
    y += capH + barH + gap;
    metricBar(m, Rect{row.x + padX, y, barW, capH}, barH, "MEM", memValue,
              ready ? gauges::fraction(static_cast<double>(sample.rssBytes),
                                       1024.0 * 1024.0 * 1024.0)
                    : 0.0,
              ready ? p.info : dim, "resident memory of this session's processes");
    y += capH + barH + gap;
    metricBar(m, Rect{row.x + padX, y, barW, capH}, barH, "GPU",
              ready ? gauges::percentLabel(gpuPct) : std::string("-"),
              ready ? gauges::fraction(gpuPct.value_or(0.0), 100.0) : 0.0,
              ready ? p.success : dim,
              ready ? (sample.gpu.valid ? "GPU engine utilization of this session"
                                        : ("GPU unknown: " + sample.gpu.reason))
                    : "this session is still starting");

    if (clicked) app.activatePane(index);
    if (rightClicked) {
        app.setContextPane(index);
        ImGui::OpenPopup("##pane_menu");
    }
    // Middle click closes (the reference window's habit). A busy pane refuses and says
    // why in the receipt, instead of orphaning its worker.
    if (hovered && middleClicked) app.closePane(index);
}

void drawSessionsPanel(App& app, const Frame& f, const Rect& box) {
    const Metrics& m = f.metrics;
    const Palette p = ui::palette();
    if (box.empty()) return;
    ui::fillRound(box, p.surface, tokens::radiusPx(m, Radius::Md));
    const Panel panel = splitPanel(m, box);
    drawPanelTitle(m, panel.head, "Sessions  (" + std::to_string(app.paneCount()) + "/5)", "", p);
    const float bw = ui::buttonWidth(m, "+ New", ui::Btn::Ghost);
    const Rect newBox{panel.head.right() - bw, panel.head.y, bw, panel.head.h};
    if (ui::buttonAt(m, newBox, "+ New", ui::Btn::Ghost,
                     "start a session in the folder the tree is showing"))
        app.startPaneHere(app.tree().root());

    if (app.paneCount() <= 0) return;
    const float rowH = f.sessionsRowH > 0.0f ? f.sessionsRowH : 2.0f * m.rowH;
    if (!ui::beginScroll("##sessions", panel.body, m, p.surface)) {
        ui::endScroll(false);
        return;
    }
    const float gap = tokens::spacePx(m, Space::Sm);
    for (int i = 0; i < app.paneCount(); ++i) {
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
        // Reserve the card before drawing into it: the card is drawn at absolute
        // rects, and moving the cursor past the content without an item is the ImGui
        // error that paints the panel red.
        ImGui::Dummy(ImVec2(w, rowH));
        drawSessionRow(app, m, Rect{origin.x, origin.y, w, rowH}, i);
        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + rowH + gap));
        ImGui::Dummy(ImVec2(w, 0.0f)); // the gap between two cards
    }
    // One menu for the card that was right-clicked (see the file header).
    if (ImGui::BeginPopup("##pane_menu")) {
        const int target = app.contextPane();
        if (target >= 0 && target < app.paneCount()) {
            ImGui::TextUnformatted(app.paneTitle(target).c_str());
            ImGui::Separator();
            if (ImGui::MenuItem("Make this the active pane", nullptr, target == app.activePane()))
                app.activatePane(target);
            if (ImGui::MenuItem("Close this pane")) app.closePane(target);
        }
        ImGui::EndPopup();
    }
    ui::endScroll(false);
}

// ---- Files ---------------------------------------------------------------

void drawFilesPanel(App& app, const Frame& f, const Rect& box) {
    const Metrics& m = f.metrics;
    const Palette p = ui::palette();
    if (box.empty()) return;
    ui::fillRound(box, p.surface, tokens::radiusPx(m, Radius::Md));
    const Panel panel = splitPanel(m, box);

    // The title carries the collapse chevron: a click folds the whole column to its
    // strip, for the frames where the transcript should have the width.
    const std::string title = app.leftOpen() ? "v  Files" : ">  Files";
    const float titleW = ui::measure(m, title, Type::Small, false) + tokens::spacePx(m, Space::Md);
    const Rect titleBox{panel.head.x, panel.head.y, std::min(titleW, panel.head.w), panel.head.h};
    if (ui::listRowAt(m, titleBox, title, false, false, Type::Small))
        app.setLeftOpen(!app.leftOpen());
    if (!app.leftOpen()) return;
    drawPanelTitle(
        m,
        Rect{panel.head.x + titleW, panel.head.y, std::max(1.0f, panel.head.w - titleW),
             panel.head.h},
        "", app.tree().rootError(), p);

    // Toolbar: parent directory, the path (Enter re-roots), "+" a session here, refresh.
    const float ctlH = m.controlH;
    const float iconW = ctlH;
    const float gap = tokens::spacePx(m, Space::Xs);
    const Rect bar{panel.body.x, panel.body.y, panel.body.w, ctlH};
    if (bar.h <= 0.0f) return;
    if (ui::iconButton(m, Rect{bar.x, bar.y, iconW, ctlH}, "..")) {
        app.setTreeRoot(std::filesystem::path(app.tree().root()).parent_path().string());
    } else if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("parent directory");
    }
    const float rightIcons = 2.0f * iconW + 2.0f * gap;
    const Rect fieldBox{bar.x + iconW + gap, bar.y,
                        std::max(20.0f, bar.w - iconW - gap - rightIcons), ctlH};
    static std::string rootEdit;
    if (rootEdit != app.tree().root()) rootEdit = app.tree().root();
    ImGui::SetCursorScreenPos(ImVec2(fieldBox.x, fieldBox.y));
    if (ui::field(m, "##root", &rootEdit, "workspace path", false, fieldBox.w))
        app.setTreeRoot(rootEdit);
    if (ui::iconButton(m, Rect{fieldBox.right() + gap, bar.y, iconW, ctlH}, "+")) {
        app.startPaneHere(app.tree().root());
    } else if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("start a session in this folder");
    }
    if (ui::iconButton(m, Rect{fieldBox.right() + gap + iconW + gap, bar.y, iconW, ctlH}, "R")) {
        app.tree().refresh();
        app.setReceipt("tree refreshed");
    } else if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("re-read the open folders");
    }

    const Rect body{panel.body.x, bar.bottom() + gap, panel.body.w,
                    std::max(0.0f, panel.body.bottom() - bar.bottom() - gap)};
    if (body.empty()) return;
    if (app.tree().root().empty()) {
        ui::textAt(m, body, "no workspace", p.textDim, Type::Small, false, 0.0f, 0.0f);
        return;
    }
    if (!ui::beginScroll("##files", body, m, p.surface)) {
        ui::endScroll(false);
        return;
    }
    const std::vector<ftree::Row> rows = app.tree().rows();
    const float iconBox = 22.0f * m.factor;
    const float iconsW = 2.0f * iconBox;
    const ToolPaths& roots = app.toolPaths();
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        const ftree::Row& row = rows[static_cast<size_t>(i)];
        const float w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(w, m.rowH)); // reserve the row (see the sessions loop)
        const Rect rowBox{origin.x, origin.y, w, m.rowH};
        const bool selected = i == app.treeSelected();
        const bool showIcons = w >= 150.0f * m.factor;
        const Rect labelBox{rowBox.x, rowBox.y, showIcons ? std::max(20.0f, w - iconsW) : w, m.rowH};
        const bool clicked = ui::listRowAt(m, labelBox, "##" + row.path, selected, false, Type::Small);
        const bool doubleClicked = ui::listRowDoubleClicked();
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            app.setContextPath(row.path, row.isDir);
            ImGui::OpenPopup("##file_menu");
        }

        std::string violation;
        const bool notAllowed = !roots.empty() && !roots.resolve(row.path, false, violation).empty();
        const float indent = tokens::spacePx(m, Space::Sm) * static_cast<float>(row.depth + 1);
        const std::string marker = row.isDir ? (row.expanded ? "v" : ">") : " ";
        ui::textAt(m,
                   Rect{labelBox.x + indent - tokens::spacePx(m, Space::Md), labelBox.y,
                        tokens::spacePx(m, Space::Md) + 2.0f, labelBox.h},
                   marker, p.textDim, Type::Small, true, 0.0f, 0.5f);
        const Rect nameBox{labelBox.x + indent, labelBox.y,
                           std::max(1.0f, labelBox.w - indent - tokens::spacePx(m, Space::Xs)),
                           labelBox.h};
        ui::textAt(m, nameBox, ui::fitLine(m, row.name, nameBox.w, Type::Small, false),
                   notAllowed ? p.textDim : (row.isDir ? p.text : p.textDim), Type::Small, false,
                   0.0f, 0.5f);
        const bool hovering = ImGui::IsMouseHoveringRect(ImVec2(rowBox.x, rowBox.y),
                                                         ImVec2(rowBox.right(), rowBox.bottom()));
        if (hovering && notAllowed)
            ImGui::SetTooltip("outside the agent's allowed roots: the path can be sent, the agent "
                              "cannot open it");

        // The two send icons: `>` plain path, `@` the context form.
        if (showIcons) {
            for (int k = 0; k < 2; ++k) {
                const Rect icon{rowBox.right() - static_cast<float>(2 - k) * iconBox, rowBox.y,
                                iconBox, m.rowH};
                ImGui::SetCursorScreenPos(ImVec2(icon.x, icon.y));
                ImGui::PushID(i * 2 + k);
                const bool hit = ImGui::InvisibleButton("##send", ImVec2(icon.w, icon.h));
                const bool hovered = ImGui::IsItemHovered();
                ImGui::PopID();
                if (hovered) ui::fillRound(icon, p.hover, tokens::radiusPx(m, Radius::Xs));
                ui::textAt(m, icon, k == 0 ? ">" : "@", p.textDim, Type::Body, true, 0.5f, 0.5f);
                if (hit) app.injectPromptText(path_text::payload(row.path, k == 1));
            }
        }

        if (clicked) {
            app.setTreeSelected(i);
            if (row.isDir) app.tree().toggle(row.path);
        }
        if (doubleClicked && row.isDir) app.setTreeRoot(row.path);
        if (doubleClicked && !row.isDir) app.injectPromptText(path_text::payload(row.path, false));

        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + m.rowH));
    }
    // One menu for the row that was right-clicked (see the file header).
    if (ImGui::BeginPopup("##file_menu")) {
        const std::string cpath = app.contextPath();
        const bool cisDir = app.contextIsDir();
        if (!cpath.empty()) {
            std::string cname = std::filesystem::path(cpath).filename().string();
            if (cname.empty()) cname = cpath;
            ImGui::TextUnformatted(cname.c_str());
            ImGui::Separator();
            if (ImGui::MenuItem("Send the path"))
                app.injectPromptText(path_text::payload(cpath, false));
            if (ImGui::MenuItem("Send @path"))
                app.injectPromptText(path_text::payload(cpath, true));
            if (ImGui::MenuItem("Copy the absolute path")) ImGui::SetClipboardText(cpath.c_str());
            if (cisDir) {
                ImGui::Separator();
                if (ImGui::MenuItem("Start a session here")) app.startPaneHere(cpath);
                if (ImGui::MenuItem("Make this the tree root")) app.setTreeRoot(cpath);
            }
        }
        ImGui::EndPopup();
    }
    ui::endScroll(false);
}

} // namespace

void drawLeftBar(App& app, const Frame& f) {
    if (!f.leftOpen || f.left.empty()) return;
    drawSessionsPanel(app, f, f.leftSessions);
    drawFilesPanel(app, f, f.leftFiles);
    // One handle between the panels. The tree takes every pixel that is left.
    if (!f.leftSplit.empty()) {
        const Metrics& m = f.metrics;
        const Palette p = ui::palette();
        const float y = f.leftSplit.y + f.leftSplit.h * 0.5f;
        ui::fill(Rect{f.leftSplit.x, y - m.hairline * 0.5f, f.leftSplit.w, m.hairline}, p.divider);
        ImGui::SetCursorScreenPos(ImVec2(f.leftSplit.x, f.leftSplit.y));
        ImGui::InvisibleButton("##split", ImVec2(f.leftSplit.w, f.leftSplit.h));
        if (ImGui::IsItemHovered() || ImGui::IsItemActive())
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    }
}

} // namespace studio
