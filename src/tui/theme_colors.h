// proJV TUI -- theme color model (ImGui-free ThemeColors).
//
// Rebuilt from legacy ui/theme.h (deleted). Field names are defined once in
// PROJV_THEME_FIELDS so struct members, JSON serialization and the theme editor
// all share the same list (no drift).
#pragma once

#include <string>
#include <vector>

#define PROJV_THEME_FIELDS(X) \
    X(windowBg) X(menuBarBg) X(frameBg) X(text) X(textDisabled) X(titleBg) \
    X(scrollbarBg) X(scrollbarGrab) X(buttonBg) X(buttonHovered) X(buttonActive) \
    X(childBg) X(border) X(separator) X(popupBg) X(headerBg) X(headerHovered) X(headerActive) \
    X(bubbleUserBg) X(bubbleAssistantBg) X(bubbleSystemBg) X(bubbleDefaultBg) X(bubbleCompactedBg) \
    X(toolTitleColor) X(toolBg) X(toolResultText) \
    X(reasoningCardBg) X(reasoningBorder) X(reasoningTextColor) X(reasoningBodyBg) X(reasoningBodyText) \
    X(phaseIdle) X(phaseStreaming) X(phaseExecutingTools) X(phaseAwaitApproval) X(phaseError) \
    X(statusTokenInfo) X(statusModelName) X(statusTokenCount) X(statusMsgCount) X(statusToolCount) \
    X(statusCtxHigh) X(statusCtxCritical) X(statusCtxMedium) X(statusCtxLow) X(statusCtxPercent) \
    X(statusRunning) X(statusAwaiting) X(statusError) X(statusWorkspace) X(statusIdle) \
    X(todoTitle) X(todoPending) X(todoDone) X(todoInProgress) X(todoOpen) X(todoEmpty) \
    X(welcomeError) X(welcomeHelp) \
    X(mdH1) X(mdH2) X(mdH3) X(mdBold) X(mdItalic) X(mdCode) X(mdLink) X(mdLinkUnder) \
    X(mdBullet) X(mdHR) X(mdCodeBg) X(mdQuote) X(mdQuoteBar) X(mdTableHdr) \
    X(clearColor)

struct ThemeColors {
    // Metadata.
    std::string name = "Obsidian";
    std::string author = "proJV";
    std::string version = "1.0";

#define X(n) std::string n;
    PROJV_THEME_FIELDS(X)
#undef X

    std::string toJson() const;
    static ThemeColors fromJson(const std::string& json);
    static ThemeColors obsidian();
    static ThemeColors light();
};

// One editable color field (name + pointer-to-member), for JSON and editor.
struct ThemeColorField {
    const char* name;
    std::string ThemeColors::*member;
};

const std::vector<ThemeColorField>& themeColorFields();

// PROJV_THEME_FIELDS is intentionally left defined so the .cpp can reuse it
// to build themeColorFields() without drifting from the struct members.
