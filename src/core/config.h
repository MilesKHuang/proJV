#pragma once
#include "models.h"
#include <string>

bool loadTomlConfig(AppConfig& cfg, const std::string& path = "");
bool saveApiKey(const std::string& key);
bool saveConfig(const AppConfig& cfg);
std::string getConfigPath();
std::string getExeDir();
std::string getProjvDir();
std::string getThemeDir();
std::string getPromptsDir();
std::string getPytoolDir();
std::string getSessionsDir();
