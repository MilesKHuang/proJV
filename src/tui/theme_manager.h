// proJV TUI -- theme manager (ImGui-free ThemeManager).
//
// Rebuilt from legacy ui/theme.cpp. All applyToImGui() calls removed; color
// application now happens in the FTXUI render layer via theme_map::hexToColor.
#pragma once

#include "theme_colors.h"

#include <functional>
#include <string>
#include <vector>

class ThemeManager {
public:
    static ThemeManager& instance();

    void init(const std::string& themeDir, const std::string& preferredName = "");
    const ThemeColors& current() const { return current_; }
    bool switchTo(const std::string& name);
    void applyCustom(const ThemeColors& tc);
    bool exportToFile(const std::string& filename) const;
    bool loadFromFile(const std::string& filepath);
    const std::vector<std::string>& installedNames() const { return installedNames_; }
    const std::string& themeDir() const { return themeDir_; }

    // Called whenever the theme changes (by name string).
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
