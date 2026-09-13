// proJV TUI -- theme manager implementation.
#include "theme_manager.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

const char* ThemeManager::builtinName0 = "Obsidian";
const char* ThemeManager::builtinName1 = "Light";

ThemeManager& ThemeManager::instance() {
    static ThemeManager mgr;
    return mgr;
}

void ThemeManager::init(const std::string& themeDir, const std::string& preferredName) {
    current_ = ThemeColors::obsidian();
    themeDir_ = themeDir;
    std::error_code ec;
    fs::create_directories(themeDir_, ec);
    if (ec) themeDir_.clear();
    if (!themeDir_.empty()) scanThemeDir();

    // If the user specified a theme in config.toml, try it first.
    if (!preferredName.empty()) {
        if (switchTo(preferredName)) return;

        std::string guessPath = themeDir_ + "/" + preferredName;
        if (preferredName.find(".json") == std::string::npos) guessPath += ".json";
        std::ifstream f(guessPath);
        if (f) {
            std::stringstream ss;
            ss << f.rdbuf();
            auto tc = ThemeColors::fromJson(ss.str());
            current_ = tc;
            installedNames_.push_back(tc.name);
            installedThemes_.push_back(tc);
            return;
        }
    }

    if (!installedThemes_.empty()) current_ = installedThemes_[0];
}

bool ThemeManager::switchTo(const std::string& name) {
    if (name == builtinName0) {
        current_ = ThemeColors::obsidian();
        if (onThemeChanged) onThemeChanged(name);
        return true;
    }
    if (name == builtinName1) {
        current_ = ThemeColors::light();
        if (onThemeChanged) onThemeChanged(name);
        return true;
    }
    for (size_t i = 0; i < installedNames_.size(); ++i) {
        if (installedNames_[i] == name) {
            current_ = installedThemes_[i];
            if (onThemeChanged) onThemeChanged(name);
            return true;
        }
    }
    return false;
}

void ThemeManager::applyCustom(const ThemeColors& tc) {
    current_ = tc;
    if (onThemeChanged) onThemeChanged(current_.name);
}

bool ThemeManager::loadFromFile(const std::string& filepath) {
    std::ifstream f(filepath);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    std::string json = ss.str();
    if (json.empty()) return false;
    auto tc = ThemeColors::fromJson(json);
    current_ = tc;

    bool found = false;
    for (const auto& n : installedNames_) if (n == tc.name) { found = true; break; }
    if (!found) {
        installedNames_.push_back(tc.name);
        installedThemes_.push_back(tc);
    }
    if (onThemeChanged) onThemeChanged(current_.name);
    return true;
}

bool ThemeManager::exportToFile(const std::string& filename) const {
    if (themeDir_.empty()) return false;
    std::string path = themeDir_ + "/" + filename;
    if (filename.find(".json") == std::string::npos) path += ".json";
    std::ofstream ofs(path);
    if (!ofs) return false;
    ofs << current_.toJson();
    return ofs.good();
}

void ThemeManager::scanThemeDir() {
    installedNames_.clear();
    installedThemes_.clear();
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(themeDir_, ec)) {
        if (ec) { ec.clear(); continue; }
        if (entry.path().extension() != ".json") continue;
        std::ifstream f(entry.path());
        if (!f) continue;
        std::stringstream ss;
        ss << f.rdbuf();
        std::string json = ss.str();
        if (json.empty()) continue;
        auto tc = ThemeColors::fromJson(json);
        installedNames_.push_back(tc.name);
        installedThemes_.push_back(std::move(tc));
    }
}
