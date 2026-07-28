#include "ui/theme.h"
#include "platform_compat.h"
#include <imgui.h>
#include <imgui_internal.h>
#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#endif
#include <cstdio>

// ============================================================================
// Color slot descriptor — one editable color field
// ============================================================================
struct ColorSlot {
    const char* label;
    std::string ThemeColors::*ptr;
};

// ============================================================================
// Category descriptor — groups related color fields
// ============================================================================
struct CategoryInfo {
    const char* name;
    std::vector<ColorSlot> slots;
};

static const std::vector<CategoryInfo>& getCategories() {
    static std::vector<CategoryInfo> cats;
    if (cats.empty()) {
        using P = std::string ThemeColors::*;
        cats = {
            {"ImGui Style", {
                {"WindowBg",       &ThemeColors::windowBg},
                {"MenuBarBg",      &ThemeColors::menuBarBg},
                {"FrameBg",        &ThemeColors::frameBg},
                {"Text",           &ThemeColors::text},
                {"TextDisabled",   &ThemeColors::textDisabled},
                {"TitleBg",        &ThemeColors::titleBg},
                {"ScrollbarBg",    &ThemeColors::scrollbarBg},
                {"ScrollbarGrab",  &ThemeColors::scrollbarGrab},
                {"ButtonBg",       &ThemeColors::buttonBg},
                {"ButtonHovered",  &ThemeColors::buttonHovered},
                {"ButtonActive",   &ThemeColors::buttonActive},
                {"ChildBg",        &ThemeColors::childBg},
                {"Border",         &ThemeColors::border},
                {"Separator",      &ThemeColors::separator},
                {"PopupBg",        &ThemeColors::popupBg},
                {"HeaderBg",       &ThemeColors::headerBg},
                {"HeaderHovered",  &ThemeColors::headerHovered},
                {"HeaderActive",   &ThemeColors::headerActive},
            }},
            {"Chat Bubbles", {
                {"UserBg",         &ThemeColors::bubbleUserBg},
                {"AssistantBg",    &ThemeColors::bubbleAssistantBg},
                {"SystemBg",       &ThemeColors::bubbleSystemBg},
                {"DefaultBg",      &ThemeColors::bubbleDefaultBg},
                {"CompactedBg",    &ThemeColors::bubbleCompactedBg},
            }},
            {"Tool Call", {
                {"TitleColor",     &ThemeColors::toolTitleColor},
                {"Bg",             &ThemeColors::toolBg},
                {"ResultText",     &ThemeColors::toolResultText},
            }},
            {"Reasoning", {
                {"CardBg",         &ThemeColors::reasoningCardBg},
                {"Border",         &ThemeColors::reasoningBorder},
                {"TextColor",      &ThemeColors::reasoningTextColor},
                {"BodyBg",         &ThemeColors::reasoningBodyBg},
                {"BodyText",       &ThemeColors::reasoningBodyText},
            }},
            {"Phase", {
                {"Idle",           &ThemeColors::phaseIdle},
                {"Streaming",      &ThemeColors::phaseStreaming},
                {"ExecutingTools", &ThemeColors::phaseExecutingTools},
                {"AwaitApproval",  &ThemeColors::phaseAwaitApproval},
                {"Error",          &ThemeColors::phaseError},
            }},
            {"Status Bar", {
                {"TokenInfo",      &ThemeColors::statusTokenInfo},
                {"ModelName",      &ThemeColors::statusModelName},
                {"TokenCount",     &ThemeColors::statusTokenCount},
                {"MsgCount",       &ThemeColors::statusMsgCount},
                {"ToolCount",      &ThemeColors::statusToolCount},
                {"CtxHigh",        &ThemeColors::statusCtxHigh},
                {"CtxCritical",    &ThemeColors::statusCtxCritical},
                {"CtxMedium",      &ThemeColors::statusCtxMedium},
                {"CtxLow",         &ThemeColors::statusCtxLow},
                {"CtxPercent",     &ThemeColors::statusCtxPercent},
                {"Running",        &ThemeColors::statusRunning},
                {"Awaiting",       &ThemeColors::statusAwaiting},
                {"Error",          &ThemeColors::statusError},
                {"Workspace",      &ThemeColors::statusWorkspace},
                {"Idle",           &ThemeColors::statusIdle},
            }},
            {"TODO", {
                {"Title",          &ThemeColors::todoTitle},
                {"Pending",        &ThemeColors::todoPending},
                {"Done",           &ThemeColors::todoDone},
                {"InProgress",     &ThemeColors::todoInProgress},
                {"Open",           &ThemeColors::todoOpen},
                {"Empty",          &ThemeColors::todoEmpty},
            }},
            {"Welcome", {
                {"Error",          &ThemeColors::welcomeError},
                {"Help",           &ThemeColors::welcomeHelp},
            }},
            {"Markdown", {
                {"H1",             &ThemeColors::mdH1},
                {"H2",             &ThemeColors::mdH2},
                {"H3",             &ThemeColors::mdH3},
                {"Bold",           &ThemeColors::mdBold},
                {"Italic",         &ThemeColors::mdItalic},
                {"Code",           &ThemeColors::mdCode},
                {"Link",           &ThemeColors::mdLink},
                {"LinkUnder",      &ThemeColors::mdLinkUnder},
                {"Bullet",         &ThemeColors::mdBullet},
                {"HR",             &ThemeColors::mdHR},
                {"CodeBg",         &ThemeColors::mdCodeBg},
                {"Quote",          &ThemeColors::mdQuote},
                {"QuoteBar",       &ThemeColors::mdQuoteBar},
                {"TableHdr",       &ThemeColors::mdTableHdr},
            }},
            {"Other", {
                {"ClearColor",     &ThemeColors::clearColor},
            }},
        };
    }
    return cats;
}

// ============================================================================
// Mini preview panel — renders a small mockup with current theme colors
// ============================================================================
static void renderMiniPreview(const ThemeColors& tc) {
    ImVec2 region = ImGui::GetContentRegionAvail();
    float w = region.x - 8.0f;

    // -- User bubble --
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ThemeColors::toVec4(tc.bubbleUserBg));
    ImGui::BeginChild("pv_user", ImVec2(w * 0.6f, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::toVec4(tc.text));
    ImGui::TextWrapped("User: Hello, this is a test message.");
    ImGui::PopStyleColor();
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // Right-align the user bubble
    // (already rendered left-aligned for simplicity)

    // -- Assistant bubble with markdown --
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ThemeColors::toVec4(tc.bubbleAssistantBg));
    ImGui::BeginChild("pv_asst", ImVec2(w, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::toVec4(tc.text));
    ImGui::TextWrapped("Assistant: Here's an example response.");
    ImGui::PopStyleColor();

    ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::toVec4(tc.mdH1));
    ImGui::TextUnformatted("## Heading");
    ImGui::PopStyleColor();

    ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::toVec4(tc.mdBold));
    ImGui::TextUnformatted("Bold text example");
    ImGui::PopStyleColor();

    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::toVec4(tc.mdCode));
    ImGui::TextUnformatted("`code`");
    ImGui::PopStyleColor();

    ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::toVec4(tc.mdQuote));
    ImGui::TextWrapped("> blockquote text");
    ImGui::PopStyleColor();

    ImGui::EndChild();
    ImGui::PopStyleColor();

    // -- Tool call bubble --
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ThemeColors::toVec4(tc.toolBg));
    ImGui::BeginChild("pv_tool", ImVec2(w, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::toVec4(tc.toolTitleColor));
    ImGui::TextUnformatted("exec_shell: g++ -std=c++20 main.cpp");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::toVec4(tc.toolResultText));
    ImGui::TextWrapped("Build successful (0 errors)");
    ImGui::PopStyleColor();
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // -- Reasoning card --
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ThemeColors::toVec4(tc.reasoningCardBg));
    ImGui::PushStyleColor(ImGuiCol_Border, ThemeColors::toVec4(tc.reasoningBorder));
    ImGui::BeginChild("pv_reason", ImVec2(w * 0.85f, 0),
        ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);
    ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::toVec4(tc.reasoningTextColor));
    ImGui::TextUnformatted("[+] Reasoning (collapsed)");
    ImGui::PopStyleColor();
    ImGui::EndChild();
    ImGui::PopStyleColor(2);

    // -- Status bar colors preview --
    ImGui::Text("Status: ");
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::toVec4(tc.statusIdle));
    ImGui::TextUnformatted("Idle");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::toVec4(tc.phaseStreaming));
    ImGui::TextUnformatted("| Streaming");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::toVec4(tc.statusError));
    ImGui::TextUnformatted("| Error");
    ImGui::PopStyleColor();

    // -- TODO colors --
    ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::toVec4(tc.todoTitle));
    ImGui::TextUnformatted("TODO:");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::toVec4(tc.todoInProgress));
    ImGui::TextUnformatted("[*]");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::toVec4(tc.todoDone));
    ImGui::TextUnformatted("[x]");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::toVec4(tc.todoOpen));
    ImGui::TextUnformatted("[ ]");
    ImGui::PopStyleColor();
}

// ============================================================================
// Main theme editor popup
// ============================================================================
void renderThemePopup() {
    ImGui::SetNextWindowSize(ImVec2(880, 560), ImGuiCond_FirstUseEver);
    if (!ImGui::BeginPopupModal("Theme Editor", nullptr)) {
        return;
    }

    ThemeManager& mgr = ThemeManager::instance();
    static ThemeColors editCopy;
    static bool initialized = false;
    if (!initialized) {
        editCopy = mgr.current();
        initialized = true;
    }

    const auto& categories = getCategories();
    static int catIdx = 0;
    if (catIdx >= (int)categories.size()) catIdx = 0;

    // ---- Top bar ----
    ImGui::SetNextItemWidth(180);
    if (ImGui::BeginCombo("##cat", categories[catIdx].name)) {
        for (int i = 0; i < (int)categories.size(); ++i) {
            if (ImGui::Selectable(categories[i].name, i == catIdx))
                catIdx = i;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    // Name field
    char nameBuf[128];
    strncpy(nameBuf, editCopy.name.c_str(), sizeof(nameBuf) - 1);
    ImGui::PushItemWidth(160);
    if (ImGui::InputText("##name", nameBuf, sizeof(nameBuf)))
        editCopy.name = nameBuf;
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("Export JSON"))
        mgr.exportToFile(editCopy.name.empty() ? "custom_theme" : editCopy.name);

    ImGui::Separator();

    // ---- Two-column layout ----
    float leftW = 370.0f;
    float rightW = ImGui::GetContentRegionAvail().x - leftW - ImGui::GetStyle().ItemSpacing.x;
    float footerH = ImGui::GetFrameHeightWithSpacing() + 4.0f;

    // Left: color slots (scrollable if needed)
    ImGui::BeginChild("left", ImVec2(leftW, 0), false, ImGuiWindowFlags_AlwaysVerticalScrollbar);
    for (const auto& slot : categories[catIdx].slots) {
        std::string& hex = editCopy.*(slot.ptr);
        char buf[10];
        strncpy(buf, hex.c_str(), sizeof(buf) - 1);

        ImGui::Text("%s", slot.label);
        ImGui::SameLine(115);
        ImGui::PushItemWidth(85);
        if (ImGui::InputText(("##hx" + std::string(slot.label)).c_str(), buf, sizeof(buf))) {
            std::string s(buf);
            if (s.size() == 7 && s[0] == '#') hex = s;
        }
        ImGui::PopItemWidth();
        ImGui::SameLine();
        ImVec4 col = ThemeColors::toVec4(hex);
        ImGui::ColorButton(("##sw" + std::string(slot.label)).c_str(), col,
            ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoTooltip, ImVec2(20, 20));
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Right: preview + buttons
    ImGui::BeginChild("right", ImVec2(rightW, 0), false);
    ImGui::Text("Preview");
    ImGui::Separator();
    ImGui::BeginChild("preview_area", ImVec2(0, -footerH), true);
    renderMiniPreview(editCopy);
    ImGui::EndChild();

    // Bottom buttons
    if (ImGui::Button("Apply", ImVec2(90, 0))) mgr.applyCustom(editCopy);
    ImGui::SameLine();
    if (ImGui::Button("Save As...", ImVec2(90, 0))) {
        std::string fname = editCopy.name.empty() ? "custom_theme" : editCopy.name;
        mgr.exportToFile(fname);
    }
    ImGui::SameLine();
    if (ImGui::Button("Load...", ImVec2(90, 0))) {
#ifdef _WIN32
        OPENFILENAMEA ofn={};
        char filename[MAX_PATH]={};
        ofn.lStructSize=sizeof(ofn);
        ofn.lpstrFilter="Theme JSON\0*.json\0All\0*.*\0";
        ofn.lpstrDefExt="json";
        ofn.lpstrFile=filename;
        ofn.nMaxFile=MAX_PATH;
        ofn.Flags=OFN_PATHMUSTEXIST|OFN_FILEMUSTEXIST;
        if(GetOpenFileNameA(&ofn)) {
            if(mgr.loadFromFile(filename)) editCopy = mgr.current();
        }
#else
        // Linux: use zenity file open dialog
        {
            std::string cmd = "zenity --file-selection --file-filter='*.json' "
                              "--title='Load Theme' 2>/dev/null";
            FILE* f = popen(cmd.c_str(), "r");
            if (f) {
                char buf[1024];
                std::string filename;
                if (fgets(buf, sizeof(buf), f)) {
                    filename = buf;
                    while (!filename.empty() && (filename.back() == '\n' || filename.back() == '\r'))
                        filename.pop_back();
                }
                pclose(f);
                if (!filename.empty()) {
                    if (mgr.loadFromFile(filename)) editCopy = mgr.current();
                }
            }
        }
#endif
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset", ImVec2(90, 0))) editCopy = ThemeColors::obsidian();
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(90, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndChild();

    ImGui::EndPopup();
}
