#pragma once
#include "models.h"
#include <string>

bool loadTomlConfig(AppConfig& cfg, const std::string& path = "");
bool saveApiKey(const std::string& key);
bool saveConfig(const AppConfig& cfg);
std::string getConfigPath();
std::string getExeDir();
// The workspace: the directory proJV was launched from (fs::current_path),
// canonicalized. Falls back to the exe dir when the launch directory cannot be
// resolved. This is the only source -- it is NOT a config key.
std::string getWorkspaceDir();
std::string getProjvDir();
std::string getThemeDir();
std::string getPromptsDir();
std::string getPytoolDir();
std::string getSessionsDir();
