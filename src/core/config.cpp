#include "config.h"
#include "toml.hpp"
#include "debug_log.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cstdlib>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

// Config lives under projv_files/ subdirectory next to the executable
static std::string resolveExePath() {
#ifdef _WIN32
    char buf[MAX_PATH];
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    return buf;
#else
    char buf[4096] = {};
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) { buf[len] = '\0'; return buf; }
    return std::filesystem::current_path().string() + "/proJV";
#endif
}

std::string getConfigPath() {
    fs::path p(resolveExePath());
    return (p.parent_path() / "projv_files" / "config.toml").string();
}

std::string getExeDir() {
    return fs::path(resolveExePath()).parent_path().string();
}

std::string getProjvDir() {
    return (fs::path(getExeDir()) / "projv_files").string();
}

std::string getThemeDir() {
    return (fs::path(getExeDir()) / "projv_files" / "theme").string();
}

std::string getPromptsDir() {
    return (fs::path(getExeDir()) / "projv_files" / "prompts").string();
}

std::string getPytoolDir() {
    return (fs::path(getExeDir()) / "projv_files" / "pytool").string();
}

std::string getSessionsDir() {
    return (fs::path(getExeDir()) / "projv_files" / "sessions").string();
}

bool loadTomlConfig(AppConfig& cfg, const std::string& path) {
    std::string configPath = path.empty() ? getConfigPath() : path;
    cfg.configPath = configPath;

    debugLogf("[Config] Loading config from: %s", configPath.c_str());

    // Ensure directory exists
    try {
        fs::create_directories(fs::path(configPath).parent_path());
    } catch (const std::exception& e) {
        debugLogf("[Config] Failed to create config dir: %s", e.what());
    }

    try {
        auto tbl = toml::parse_file(configPath);

        cfg.apiKey = tbl["api_key"].value_or<std::string>("");
        cfg.model = tbl["model"].value_or<std::string>("deepseek-v4-flash");
        cfg.baseUrl = tbl["base_url"].value_or<std::string>("https://api.deepseek.com");
        cfg.maxTokens = tbl["max_tokens"].value_or<int>(4096);
        cfg.temperature = tbl["temperature"].value_or<double>(0.0);
        cfg.workspacePath = tbl["workspace_path"].value_or<std::string>("");
        cfg.cppCompilerPath = tbl["cpp_compiler_path"].value_or<std::string>("");
        cfg.pythonPath = tbl["python_path"].value_or<std::string>("");
        cfg.contextWindow = tbl["context_window"].value_or<size_t>(0);
        cfg.themeName = tbl["theme"].value_or<std::string>("");

        // Parse [model_prices] section (optional)
        if (auto* pricesTbl = tbl["model_prices"].as_table()) {
            for (auto& [key, val] : *pricesTbl) {
                if (val.is_table()) {
                    ModelPriceEntry entry;
                    entry.prefix = key;
                    auto& priceTbl = *val.as_table();
                    entry.inputPrice = priceTbl["input"].value_or(0.5);
                    entry.outputPrice = priceTbl["output"].value_or(2.0);
                    cfg.modelPrices.push_back(entry);
                }
            }
            debugLogf("[Config] Loaded %zu model price entries", cfg.modelPrices.size());
        }

        debugLogf("[Config] Loaded: apiKey=%s model=%s baseUrl=%s maxTokens=%d temperature=%.1f workspace=%s cpp=%s python=%s",
            cfg.apiKey.empty() ? "(empty)" : "***",
            cfg.model.c_str(), cfg.baseUrl.c_str(),
            cfg.maxTokens, cfg.temperature,
            cfg.workspacePath.empty() ? "(default=exe dir)" : cfg.workspacePath.c_str(),
            cfg.cppCompilerPath.empty() ? "(not set)" : cfg.cppCompilerPath.c_str(),
            cfg.pythonPath.empty() ? "(not set)" : cfg.pythonPath.c_str());
    }
    catch (const toml::parse_error& e) {
        debugLogf("[Config] parse_error: %s", e.what());
        cfg.loadError = "No config file found at " + configPath + ". Set API key in the Settings dialog.";
        return false;
    }
    catch (const std::exception& e) {
        debugLogf("[Config] Unexpected exception loading config: %s", e.what());
        cfg.loadError = std::string("Error loading config: ") + e.what();
        return false;
    }

    // Also try DEEPSEEK_API_KEY env var (but config file takes priority)
    if (cfg.apiKey.empty()) {
        const char* envKey = std::getenv("DEEPSEEK_API_KEY");
        if (envKey && *envKey) {
            cfg.apiKey = envKey;
            debugLog("[Config] Using DEEPSEEK_API_KEY env var");
        }
    }

    cfg.loaded = !cfg.apiKey.empty();

    if (cfg.apiKey.empty()) {
        cfg.loadError = "No API key configured. Set it in Settings → Configuration.";
        debugLog("[Config] No API key found");
    } else {
        debugLog("[Config] Config loaded successfully");
    }

    return cfg.loaded;
}

bool saveApiKey(const std::string& key) {
    std::string configPath = getConfigPath();
    debugLogf("[Config] saveApiKey to: %s", configPath.c_str());

    // Ensure directory exists
    try {
        fs::create_directories(fs::path(configPath).parent_path());
    } catch (const std::exception& e) {
        debugLogf("[Config] Failed to create config dir: %s", e.what());
        return false;
    }

    // Try to read existing config first
    toml::table tbl;
    try {
        tbl = toml::parse_file(configPath);
        debugLog("[Config] Existing config found, updating");
    } catch (const toml::parse_error&) {
        debugLog("[Config] No existing config, creating new");
    } catch (const std::exception& e) {
        debugLogf("[Config] Error reading existing config: %s", e.what());
    }

    // Update values -- default to blanks so existing keys aren't wiped on pure API-key saves
    try {
        tbl.insert_or_assign("api_key", key);
        if (!tbl.contains("model"))
            tbl.insert_or_assign("model", "deepseek-v4-flash");
        if (!tbl.contains("base_url"))
            tbl.insert_or_assign("base_url", "https://api.deepseek.com");
        if (!tbl.contains("max_tokens"))
            tbl.insert_or_assign("max_tokens", 4096);
        if (!tbl.contains("temperature"))
            tbl.insert_or_assign("temperature", 0.0);
        if (!tbl.contains("workspace_path"))
            tbl.insert_or_assign("workspace_path", "");
        if (!tbl.contains("cpp_compiler_path"))
            tbl.insert_or_assign("cpp_compiler_path", "");
        if (!tbl.contains("python_path"))
            tbl.insert_or_assign("python_path", "");
        if (!tbl.contains("context_window")) tbl.insert_or_assign("context_window", 0);
    } catch (const std::exception& e) {
        debugLogf("[Config] Error updating config table: %s", e.what());
        return false;
    }

    // Write back
    std::ofstream ofs(configPath);
    if (!ofs) {
        debugLogf("[Config] Failed to open file for writing: %s", configPath.c_str());
        return false;
    }

    try {
        ofs << "# proJV configuration\n\n";
        ofs << tbl << "\n";
    } catch (const std::exception& e) {
        debugLogf("[Config] Error writing config: %s", e.what());
        return false;
    }

    bool ok = ofs.good();
    debugLogf("[Config] Config saved: %s", ok ? "OK" : "FAIL");
    return ok;
}

bool saveConfig(const AppConfig& cfg) {
    std::string configPath = getConfigPath();
    toml::table tbl;
    try {
        tbl = toml::parse_file(configPath);
    } catch (...) {}

    // * Always overwrite with current AppConfig values.
    //   Previously used if(!tbl.contains()) -- that silently discarded UI edits.
    tbl.insert_or_assign("api_key", cfg.apiKey);
    tbl.insert_or_assign("model", cfg.model);
    tbl.insert_or_assign("base_url", cfg.baseUrl);
    tbl.insert_or_assign("max_tokens", cfg.maxTokens);
    tbl.insert_or_assign("temperature", cfg.temperature);
    tbl.insert_or_assign("workspace_path", cfg.workspacePath);
    tbl.insert_or_assign("cpp_compiler_path", cfg.cppCompilerPath);
    tbl.insert_or_assign("python_path", cfg.pythonPath);
    tbl.insert_or_assign("context_window", static_cast<int64_t>(cfg.contextWindow));
    if (!cfg.themeName.empty())
        tbl.insert_or_assign("theme", cfg.themeName);

    std::ofstream ofs(configPath);
    if (!ofs) return false;
    ofs << "# proJV configuration\n\n";
    ofs << tbl << "\n";
    return ofs.good();
}