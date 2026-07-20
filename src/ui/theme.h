#pragma once
#include <imgui.h>
#include <string>
#include <vector>
#include <cstdint>
#include <functional>

// ============================================================================
// ThemeColors — centralized color palette for the entire application
// ============================================================================
struct ThemeColors {
    // ---- Metadata ----
    std::string name    = "Obsidian";
    std::string author  = "proJV";
    std::string version = "1.0";

    // ---- ImGui Global Style (mapped to ImGuiCol_*) ----
    std::string windowBg       = "#121218";
    std::string menuBarBg      = "#0C0C12";
    std::string frameBg        = "#1A1A24";
    std::string text           = "#D4D4E0";
    std::string textDisabled   = "#585868";
    std::string titleBg        = "#0C0C12";
    std::string scrollbarBg    = "#0C0C12";
    std::string scrollbarGrab  = "#3A3A50";
    std::string buttonBg       = "#1A1A24";
    std::string buttonHovered  = "#28283A";
    std::string buttonActive   = "#3A3A55";
    std::string childBg        = "#121218";
    std::string border         = "#28283A";
    std::string separator      = "#28283A";
    std::string popupBg        = "#161620";
    std::string headerBg       = "#1A1A24";
    std::string headerHovered  = "#28283A";
    std::string headerActive   = "#3A3A55";

    // ---- Chat Bubbles ----
    std::string bubbleUserBg      = "#1E2840";
    std::string bubbleAssistantBg = "#1E281E";
    std::string bubbleSystemBg    = "#181820";
    std::string bubbleDefaultBg   = "#14141C";
    std::string bubbleCompactedBg = "#2A2418";

    // ---- Tool Call Bubbles ----
    std::string toolTitleColor   = "#E0A040";
    std::string toolBg           = "#1C1C2A";
    std::string toolResultText   = "#A0A0B8";

    // ---- Reasoning (Chain-of-Thought) Card ----
    std::string reasoningCardBg    = "#181828";
    std::string reasoningBorder    = "#484880";
    std::string reasoningTextColor = "#A0A0E0";
    std::string reasoningBodyBg    = "#101020";
    std::string reasoningBodyText  = "#9090D0";

    // ---- Agent Phase Indicators ----
    std::string phaseIdle           = "#686888";
    std::string phaseStreaming      = "#E0C860";
    std::string phaseExecutingTools = "#40C8A0";
    std::string phaseAwaitApproval  = "#E06060";
    std::string phaseError          = "#E03040";

    // ---- Status Bar ----
    std::string statusTokenInfo   = "#686888";
    std::string statusModelName   = "#686888";
    std::string statusTokenCount  = "#484868";
    std::string statusMsgCount    = "#484868";
    std::string statusToolCount   = "#A0A848";
    std::string statusCtxHigh     = "#E08830";
    std::string statusCtxCritical = "#E03040";
    std::string statusCtxMedium   = "#C8C040";
    std::string statusCtxLow      = "#40C880";
    std::string statusCtxPercent  = "#484868";
    std::string statusRunning     = "#E08830";
    std::string statusAwaiting    = "#E06060";
    std::string statusError       = "#E03040";
    std::string statusWorkspace   = "#383858";
    std::string statusIdle        = "#686888";

    // ---- TODO Panel ----
    std::string todoTitle      = "#E0C860";
    std::string todoPending    = "#A0A0B8";
    std::string todoDone       = "#40C880";
    std::string todoInProgress = "#E08830";
    std::string todoOpen       = "#686888";
    std::string todoEmpty      = "#383858";

    // ---- Welcome Page ----
    std::string welcomeError = "#E03040";
    std::string welcomeHelp  = "#585868";

    // ---- Markdown ----
    std::string mdH1        = "#E0B840";
    std::string mdH2        = "#E0A840";
    std::string mdH3        = "#D0C070";
    std::string mdBold      = "#E0B840";
    std::string mdItalic    = "#70B8D8";
    std::string mdCode      = "#E07850";
    std::string mdLink      = "#6090E0";
    std::string mdLinkUnder = "#4068C0";
    std::string mdBullet    = "#70A8C8";
    std::string mdHR        = "#303048";
    std::string mdCodeBg    = "#181828";
    std::string mdQuote     = "#8888A0";
    std::string mdQuoteBar  = "#5858A0";
    std::string mdTableHdr  = "#1E1E2C";

    // ---- D3D Clear Color ----
    std::string clearColor = "#0A0A10";

    // ---- Converters ----
    static ImVec4 toVec4(const std::string& hex);
    static ImU32  toU32 (const std::string& hex);

    // ---- Bulk apply to ImGui style ----
    void applyToImGui() const;

    // ---- Serialization ----
    std::string toJson() const;
    static ThemeColors fromJson(const std::string& json);
    static ThemeColors obsidian();
    static ThemeColors light();
};

// ============================================================================
// ThemeManager — singleton managing current theme + installed themes
// ============================================================================
class ThemeManager {
public:
    static ThemeManager& instance();

    void init(const std::string& exeDir, const std::string& preferredName = "");
    const ThemeColors& current() const { return current_; }
    bool switchTo(const std::string& name);
    void applyCustom(const ThemeColors& tc);
    bool exportToFile(const std::string& filename) const;
    bool loadFromFile(const std::string& filepath);
    const std::vector<std::string>& installedNames() const { return installedNames_; }
    const std::string& themeDir() const { return themeDir_; }

    // Called whenever the theme changes (by name string).
    // App sets this to save the preference to config.toml.
    std::function<void(const std::string& themeName)> onThemeChanged;

    static const char* builtinName0;  // "Obsidian"
    static const char* builtinName1;  // "Light"

private:
    ThemeManager() = default;
    ThemeColors current_;
    std::string themeDir_;
    std::vector<std::string> installedNames_;
    std::vector<ThemeColors> installedThemes_;
    void scanThemeDir();
};

// ============================================================================
// Theme editor popup (defined in theme_popup.cpp)
// ============================================================================
void renderThemePopup();

