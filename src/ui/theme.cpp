#include "ui/theme.h"
#include "json.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstring>

namespace fs = std::filesystem;

ImVec4 ThemeColors::toVec4(const std::string& hex) {
    if (hex.size() < 7 || hex[0] != '#') return ImVec4(1, 1, 1, 1);
    unsigned int r, g, b;
    try {
        r = std::stoul(hex.substr(1, 2), nullptr, 16);
        g = std::stoul(hex.substr(3, 2), nullptr, 16);
        b = std::stoul(hex.substr(5, 2), nullptr, 16);
    } catch (...) { return ImVec4(1, 1, 1, 1); }
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
}

ImU32 ThemeColors::toU32(const std::string& hex) {
    return ImGui::ColorConvertFloat4ToU32(toVec4(hex));
}

static void setCol(ImGuiStyle& s, ImGuiCol_ idx, const std::string& hex) {
    s.Colors[idx] = ThemeColors::toVec4(hex);
}

void ThemeColors::applyToImGui() const {
    ImGuiStyle& s = ImGui::GetStyle();
    setCol(s, ImGuiCol_WindowBg,       windowBg);
    setCol(s, ImGuiCol_MenuBarBg,      menuBarBg);
    setCol(s, ImGuiCol_FrameBg,        frameBg);
    setCol(s, ImGuiCol_Text,           text);
    setCol(s, ImGuiCol_TextDisabled,   textDisabled);
    setCol(s, ImGuiCol_TitleBg,        titleBg);
    setCol(s, ImGuiCol_ScrollbarBg,    scrollbarBg);
    setCol(s, ImGuiCol_ScrollbarGrab,  scrollbarGrab);
    setCol(s, ImGuiCol_Button,         buttonBg);
    setCol(s, ImGuiCol_ButtonHovered,  buttonHovered);
    setCol(s, ImGuiCol_ButtonActive,   buttonActive);
    setCol(s, ImGuiCol_ChildBg,        childBg);
    setCol(s, ImGuiCol_Border,         border);
    setCol(s, ImGuiCol_Separator,      separator);
    setCol(s, ImGuiCol_PopupBg,        popupBg);
    setCol(s, ImGuiCol_Header,         headerBg);
    setCol(s, ImGuiCol_HeaderHovered,  headerHovered);
    setCol(s, ImGuiCol_HeaderActive,   headerActive);
    setCol(s, ImGuiCol_FrameBgHovered, frameBg);
    setCol(s, ImGuiCol_FrameBgActive,  buttonHovered);
    setCol(s, ImGuiCol_CheckMark,      text);
    setCol(s, ImGuiCol_SliderGrab,     scrollbarGrab);
    setCol(s, ImGuiCol_SliderGrabActive, text);
    setCol(s, ImGuiCol_ResizeGrip,     scrollbarBg);
    setCol(s, ImGuiCol_ResizeGripHovered, scrollbarGrab);
    setCol(s, ImGuiCol_ResizeGripActive,  text);
    setCol(s, ImGuiCol_Tab,            childBg);
    setCol(s, ImGuiCol_TabHovered,     headerHovered);
    setCol(s, ImGuiCol_TabActive,      headerActive);
    setCol(s, ImGuiCol_TableHeaderBg,  headerBg);
    setCol(s, ImGuiCol_TableBorderStrong, border);
    setCol(s, ImGuiCol_TableBorderLight,  separator);
    setCol(s, ImGuiCol_TableRowBg,     childBg);
    setCol(s, ImGuiCol_TableRowBgAlt,  windowBg);
    setCol(s, ImGuiCol_NavHighlight,   headerActive);
}
#define TC_JSON_GET(f) if(c.contains(#f)&&c[#f].is_string())tc.f=c[#f].get<std::string>();

std::string ThemeColors::toJson() const {
    nlohmann::json j;
    j["name"]=name; j["author"]=author; j["version"]=version;
    nlohmann::json& c=j["colors"];
    c["windowBg"]=windowBg; c["menuBarBg"]=menuBarBg; c["frameBg"]=frameBg;
    c["text"]=text; c["textDisabled"]=textDisabled; c["titleBg"]=titleBg;
    c["scrollbarBg"]=scrollbarBg; c["scrollbarGrab"]=scrollbarGrab;
    c["buttonBg"]=buttonBg; c["buttonHovered"]=buttonHovered; c["buttonActive"]=buttonActive;
    c["childBg"]=childBg; c["border"]=border; c["separator"]=separator;
    c["popupBg"]=popupBg; c["headerBg"]=headerBg;
    c["headerHovered"]=headerHovered; c["headerActive"]=headerActive;
    c["bubbleUserBg"]=bubbleUserBg; c["bubbleAssistantBg"]=bubbleAssistantBg;
    c["bubbleSystemBg"]=bubbleSystemBg; c["bubbleDefaultBg"]=bubbleDefaultBg;
    c["bubbleCompactedBg"]=bubbleCompactedBg;
    c["toolTitleColor"]=toolTitleColor; c["toolBg"]=toolBg; c["toolResultText"]=toolResultText;
    c["reasoningCardBg"]=reasoningCardBg; c["reasoningBorder"]=reasoningBorder;
    c["reasoningTextColor"]=reasoningTextColor; c["reasoningBodyBg"]=reasoningBodyBg;
    c["reasoningBodyText"]=reasoningBodyText;
    c["phaseIdle"]=phaseIdle; c["phaseStreaming"]=phaseStreaming;
    c["phaseExecutingTools"]=phaseExecutingTools; c["phaseAwaitApproval"]=phaseAwaitApproval;
    c["phaseError"]=phaseError;
    c["statusTokenInfo"]=statusTokenInfo; c["statusModelName"]=statusModelName;
    c["statusTokenCount"]=statusTokenCount; c["statusMsgCount"]=statusMsgCount;
    c["statusToolCount"]=statusToolCount; c["statusCtxHigh"]=statusCtxHigh;
    c["statusCtxCritical"]=statusCtxCritical; c["statusCtxMedium"]=statusCtxMedium;
    c["statusCtxLow"]=statusCtxLow; c["statusCtxPercent"]=statusCtxPercent;
    c["statusRunning"]=statusRunning; c["statusAwaiting"]=statusAwaiting;
    c["statusError"]=statusError; c["statusWorkspace"]=statusWorkspace;
    c["statusIdle"]=statusIdle;
    c["todoTitle"]=todoTitle; c["todoPending"]=todoPending; c["todoDone"]=todoDone;
    c["todoInProgress"]=todoInProgress; c["todoOpen"]=todoOpen; c["todoEmpty"]=todoEmpty;
    c["welcomeError"]=welcomeError; c["welcomeHelp"]=welcomeHelp;
    c["mdH1"]=mdH1; c["mdH2"]=mdH2; c["mdH3"]=mdH3;
    c["mdBold"]=mdBold; c["mdItalic"]=mdItalic; c["mdCode"]=mdCode;
    c["mdLink"]=mdLink; c["mdLinkUnder"]=mdLinkUnder; c["mdBullet"]=mdBullet;
    c["mdHR"]=mdHR; c["mdCodeBg"]=mdCodeBg; c["mdQuote"]=mdQuote;
    c["mdQuoteBar"]=mdQuoteBar; c["mdTableHdr"]=mdTableHdr;
    c["clearColor"]=clearColor;
    return j.dump(2);
}

ThemeColors ThemeColors::fromJson(const std::string& json) {
    ThemeColors tc=obsidian();
    try{
        nlohmann::json j=nlohmann::json::parse(json);
        if(j.contains("name")&&j["name"].is_string()) tc.name=j["name"].get<std::string>();
        if(j.contains("author")&&j["author"].is_string()) tc.author=j["author"].get<std::string>();
        if(j.contains("version")&&j["version"].is_string()) tc.version=j["version"].get<std::string>();
        if(j.contains("colors")&&j["colors"].is_object()){
            auto& c=j["colors"];
            TC_JSON_GET(windowBg);TC_JSON_GET(menuBarBg);TC_JSON_GET(frameBg);
            TC_JSON_GET(text);TC_JSON_GET(textDisabled);TC_JSON_GET(titleBg);
            TC_JSON_GET(scrollbarBg);TC_JSON_GET(scrollbarGrab);
            TC_JSON_GET(buttonBg);TC_JSON_GET(buttonHovered);TC_JSON_GET(buttonActive);
            TC_JSON_GET(childBg);TC_JSON_GET(border);TC_JSON_GET(separator);
            TC_JSON_GET(popupBg);TC_JSON_GET(headerBg);
            TC_JSON_GET(headerHovered);TC_JSON_GET(headerActive);
            TC_JSON_GET(bubbleUserBg);TC_JSON_GET(bubbleAssistantBg);
            TC_JSON_GET(bubbleSystemBg);TC_JSON_GET(bubbleDefaultBg);
            TC_JSON_GET(bubbleCompactedBg);
            TC_JSON_GET(toolTitleColor);TC_JSON_GET(toolBg);TC_JSON_GET(toolResultText);
            TC_JSON_GET(reasoningCardBg);TC_JSON_GET(reasoningBorder);
            TC_JSON_GET(reasoningTextColor);TC_JSON_GET(reasoningBodyBg);
            TC_JSON_GET(reasoningBodyText);
            TC_JSON_GET(phaseIdle);TC_JSON_GET(phaseStreaming);
            TC_JSON_GET(phaseExecutingTools);TC_JSON_GET(phaseAwaitApproval);
            TC_JSON_GET(phaseError);
            TC_JSON_GET(statusTokenInfo);TC_JSON_GET(statusModelName);
            TC_JSON_GET(statusTokenCount);TC_JSON_GET(statusMsgCount);
            TC_JSON_GET(statusToolCount);TC_JSON_GET(statusCtxHigh);
            TC_JSON_GET(statusCtxCritical);TC_JSON_GET(statusCtxMedium);
            TC_JSON_GET(statusCtxLow);TC_JSON_GET(statusCtxPercent);
            TC_JSON_GET(statusRunning);TC_JSON_GET(statusAwaiting);
            TC_JSON_GET(statusError);TC_JSON_GET(statusWorkspace);
            TC_JSON_GET(statusIdle);
            TC_JSON_GET(todoTitle);TC_JSON_GET(todoPending);TC_JSON_GET(todoDone);
            TC_JSON_GET(todoInProgress);TC_JSON_GET(todoOpen);TC_JSON_GET(todoEmpty);
            TC_JSON_GET(welcomeError);TC_JSON_GET(welcomeHelp);
            TC_JSON_GET(mdH1);TC_JSON_GET(mdH2);TC_JSON_GET(mdH3);
            TC_JSON_GET(mdBold);TC_JSON_GET(mdItalic);TC_JSON_GET(mdCode);
            TC_JSON_GET(mdLink);TC_JSON_GET(mdLinkUnder);TC_JSON_GET(mdBullet);
            TC_JSON_GET(mdHR);TC_JSON_GET(mdCodeBg);TC_JSON_GET(mdQuote);
            TC_JSON_GET(mdQuoteBar);TC_JSON_GET(mdTableHdr);
            TC_JSON_GET(clearColor);
        }
    }catch(...){}
    return tc;
}
#undef TC_JSON_GET
ThemeColors ThemeColors::obsidian() { return ThemeColors{}; }

ThemeColors ThemeColors::light() {
    ThemeColors tc;
    tc.name="Light";
    tc.windowBg="#F5F5F5"; tc.menuBarBg="#E8E8E8"; tc.frameBg="#FFFFFF";
    tc.text="#1A1A1E"; tc.textDisabled="#9999A0"; tc.titleBg="#E0E0E0";
    tc.scrollbarBg="#E8E8E8"; tc.scrollbarGrab="#C0C0C8";
    tc.buttonBg="#E8E8EC"; tc.buttonHovered="#D0D0D8"; tc.buttonActive="#B8B8C0";
    tc.childBg="#F5F5F5"; tc.border="#D0D0D8"; tc.separator="#D0D0D8";
    tc.popupBg="#FAFAFA"; tc.headerBg="#EBEBF0";
    tc.headerHovered="#D8D8E0"; tc.headerActive="#C0C0CC";
    tc.bubbleUserBg="#DCE8F0"; tc.bubbleAssistantBg="#E2ECD8";
    tc.bubbleSystemBg="#E8E8EC"; tc.bubbleDefaultBg="#EEEEF0"; tc.bubbleCompactedBg="#FFF3CD";
    tc.toolTitleColor="#CC7A00"; tc.toolBg="#EDEDF2"; tc.toolResultText="#555560";
    tc.reasoningCardBg="#EDE8F8"; tc.reasoningBorder="#B8A0D8";
    tc.reasoningTextColor="#553388"; tc.reasoningBodyBg="#F5F0FC"; tc.reasoningBodyText="#664499";
    tc.phaseIdle="#777780"; tc.phaseStreaming="#CC8800"; tc.phaseExecutingTools="#228855";
    tc.phaseAwaitApproval="#CC3333"; tc.phaseError="#CC0000";
    tc.statusTokenInfo="#777780"; tc.statusModelName="#777780";
    tc.statusTokenCount="#555560"; tc.statusMsgCount="#555560"; tc.statusToolCount="#888833";
    tc.statusCtxHigh="#E67800"; tc.statusCtxCritical="#CC0000"; tc.statusCtxMedium="#AAAA33";
    tc.statusCtxLow="#228855"; tc.statusCtxPercent="#555560";
    tc.statusRunning="#CC7A00"; tc.statusAwaiting="#CC3333"; tc.statusError="#CC0000";
    tc.statusWorkspace="#666670"; tc.statusIdle="#777780";
    tc.todoTitle="#CC8800"; tc.todoPending="#555560"; tc.todoDone="#228855";
    tc.todoInProgress="#CC8800"; tc.todoOpen="#777780"; tc.todoEmpty="#9999A0";
    tc.welcomeError="#CC0000"; tc.welcomeHelp="#888890";
    tc.mdH1="#CC6600"; tc.mdH2="#CC7700"; tc.mdH3="#B89030";
    tc.mdBold="#CC7700"; tc.mdItalic="#3377AA"; tc.mdCode="#CC5533";
    tc.mdLink="#2266CC"; tc.mdLinkUnder="#1144AA"; tc.mdBullet="#4488BB";
    tc.mdHR="#C0C0C8"; tc.mdCodeBg="#EEEEF2"; tc.mdQuote="#666670";
    tc.mdQuoteBar="#4488BB"; tc.mdTableHdr="#E0E0E8";
    tc.clearColor="#F0F0F0";
    return tc;
}

const char* ThemeManager::builtinName0 = "Obsidian";
const char* ThemeManager::builtinName1 = "Light";

ThemeManager& ThemeManager::instance() { static ThemeManager mgr; return mgr; }

void ThemeManager::init(const std::string& themeDir, const std::string& preferredName) {
    current_=ThemeColors::obsidian();
    themeDir_=themeDir;
    std::error_code ec;
    fs::create_directories(themeDir_, ec);
    if(ec) themeDir_.clear();
    if(!themeDir_.empty()) scanThemeDir();

    // If user specified a theme in config.toml, try to use it first
    if(!preferredName.empty()) {
        if(switchTo(preferredName)) {
            // Successfully switched — already applied
            return;
        }
        // Preferred theme not found — try loading from file directly
        std::string guessPath = themeDir_ + "/" + preferredName;
        if(preferredName.find(".json")==std::string::npos) guessPath+=".json";
        std::ifstream f(guessPath);
        if(f) {
            std::stringstream ss; ss<<f.rdbuf();
            try {
                auto tc=ThemeColors::fromJson(ss.str());
                current_=tc;
                installedNames_.push_back(tc.name);
                installedThemes_.push_back(tc);
                current_.applyToImGui();
                return;
            }catch(...){}
        }
    }

    // No preference or preference not found — use first installed or default
    if(!installedThemes_.empty()) current_=installedThemes_[0];
    current_.applyToImGui();
}

bool ThemeManager::switchTo(const std::string& name) {
    if(name==builtinName0){ current_=ThemeColors::obsidian(); current_.applyToImGui(); if(onThemeChanged)onThemeChanged(name); return true; }
    if(name==builtinName1){ current_=ThemeColors::light(); current_.applyToImGui(); if(onThemeChanged)onThemeChanged(name); return true; }
    for(size_t i=0;i<installedNames_.size();++i){
        if(installedNames_[i]==name){ current_=installedThemes_[i]; current_.applyToImGui(); if(onThemeChanged)onThemeChanged(name); return true; }
    }
    return false;
}

void ThemeManager::applyCustom(const ThemeColors& tc) { current_=tc; current_.applyToImGui(); if(onThemeChanged)onThemeChanged(current_.name); }

bool ThemeManager::loadFromFile(const std::string& filepath) {
    std::ifstream f(filepath);
    if(!f) return false;
    std::stringstream ss; ss<<f.rdbuf();
    std::string json=ss.str();
    if(json.empty()) return false;
    try {
        auto tc=ThemeColors::fromJson(json);
        current_=tc;
        current_.applyToImGui();
        // Also add to installed list if not already there
        bool found=false;
        for(auto& n:installedNames_) if(n==tc.name){found=true;break;}
        if(!found){
            installedNames_.push_back(tc.name);
            installedThemes_.push_back(tc);
        }
        if(onThemeChanged)onThemeChanged(current_.name);
        return true;
    }catch(...){ return false; }
}

bool ThemeManager::exportToFile(const std::string& filename) const {
    if(themeDir_.empty()) return false;
    std::string path=themeDir_+"/"+filename;
    if(filename.find(".json")==std::string::npos) path+=".json";
    std::ofstream ofs(path);
    if(!ofs) return false;
    ofs<<current_.toJson();
    return ofs.good();
}

void ThemeManager::scanThemeDir() {
    installedNames_.clear(); installedThemes_.clear();
    std::error_code ec;
    for(auto& entry: fs::directory_iterator(themeDir_,ec)){
        if(ec){ec.clear();continue;}
        if(entry.path().extension()!=".json") continue;
        std::ifstream f(entry.path());
        if(!f) continue;
        std::stringstream ss; ss<<f.rdbuf();
        std::string json=ss.str();
        if(json.empty()) continue;
        try{
            auto tc=ThemeColors::fromJson(json);
            installedNames_.push_back(tc.name);
            installedThemes_.push_back(std::move(tc));
        }catch(...){}
    }
}
