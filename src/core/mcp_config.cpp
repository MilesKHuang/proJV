#include "core/mcp_config.h"

#include "core/config.h"
#include "debug_log.h"
#include "json.hpp"

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace mcp {
namespace {

// Seeded once, on first run. Every field is listed here because this file is the
// only documentation a user reads before editing it. NOTE: change it and send the
// next message -- the list is reloaded before the turn starts (no restart).
constexpr const char* kTemplate =
    "{\n"
    "  \"_comment\": \"Add servers below, save, then send your next message: the list is "
    "reloaded before the turn starts. Fields: name, description (one line, what it is "
    "for -- required), command, args, env, protocol (stateful|stateless), trusted, "
    "enabled, timeout_ms.\",\n"
    "  \"servers\": []\n"
    "}\n";

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Accept both ["a","b"] and ["-y","pkg"]. A bare string is a config mistake
// worth reporting instead of silently treating it as one argument.
bool readArgs(const nlohmann::json& j, std::vector<std::string>& out, std::string& why) {
    out.clear();
    if (!j.is_array()) {
        why = "'args' must be an array of strings";
        return false;
    }
    for (const auto& item : j) {
        if (!item.is_string()) {
            why = "'args' must contain only strings";
            return false;
        }
        out.push_back(item.get<std::string>());
    }
    return true;
}

bool readEnv(const nlohmann::json& j, std::map<std::string, std::string>& out, std::string& why) {
    out.clear();
    if (!j.is_object()) {
        why = "'env' must be an object of string values";
        return false;
    }
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (!it.value().is_string()) {
            why = "'env." + it.key() + "' must be a string";
            return false;
        }
        out[it.key()] = it.value().get<std::string>();
    }
    return true;
}

} // namespace

std::string configPath() {
    return (fs::path(getProjvDir()) / "mcp.json").string();
}

bool ensureConfigFile(const std::string& path) {
    std::error_code ec;
    if (fs::exists(path, ec)) return true;
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        debugLogf("[mcp] could not create '%s'", path.c_str());
        return false;
    }
    f << kTemplate;
    debugLogf("[mcp] created empty server list at '%s'", path.c_str());
    return true;
}

bool loadConfig(const std::string& path, std::vector<McpServerConfig>& out, std::string& err) {
    out.clear();
    err.clear();

    std::ifstream f(path, std::ios::binary);
    if (!f) {
        err = "cannot open " + path;
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string text = ss.str();
    if (trim(text).empty()) {
        debugLogf("[mcp] '%s' is empty; no servers", path.c_str());
        return true;
    }

    nlohmann::json root;
    try {
        root = nlohmann::json::parse(text);
    } catch (const std::exception& e) {
        err = std::string("invalid JSON: ") + e.what();
        return false;
    }
    if (!root.is_object() || !root.contains("servers") || !root["servers"].is_array()) {
        err = "expected an object with a \"servers\" array";
        return false;
    }

    std::set<std::string> seen;
    int index = 0;
    for (const auto& item : root["servers"]) {
        ++index;
        if (!item.is_object()) {
            debugLogf("[mcp] server #%d skipped: not an object", index);
            continue;
        }
        McpServerConfig cfg;
        cfg.name = trim(item.value("name", std::string()));
        if (cfg.name.empty()) {
            debugLogf("[mcp] server #%d skipped: missing 'name'", index);
            continue;
        }
        if (!seen.insert(cfg.name).second) {
            debugLogf("[mcp] server '%s' skipped: duplicate name (first one wins)",
                      cfg.name.c_str());
            continue;
        }
        cfg.command = trim(item.value("command", std::string()));
        if (cfg.command.empty()) {
            debugLogf("[mcp] server '%s' skipped: missing 'command'", cfg.name.c_str());
            continue;
        }
        // Not fatal (an old file must keep working), but loud: the description is
        // the only thing that tells the model what this server is for.
        cfg.description = trim(item.value("description", std::string()));
        if (cfg.description.empty())
            debugLogf("[mcp] server '%s' has no 'description': the model will not know what it "
                      "is for",
                      cfg.name.c_str());

        std::string why;
        if (item.contains("args") && !readArgs(item["args"], cfg.args, why)) {
            debugLogf("[mcp] server '%s' skipped: %s", cfg.name.c_str(), why.c_str());
            continue;
        }
        if (item.contains("env") && !readEnv(item["env"], cfg.env, why)) {
            debugLogf("[mcp] server '%s' skipped: %s", cfg.name.c_str(), why.c_str());
            continue;
        }

        cfg.protocol = item.value("protocol", std::string("stateful"));
        if (cfg.protocol != "stateful" && cfg.protocol != "stateless") {
            debugLogf("[mcp] server '%s' skipped: protocol must be \"stateful\" or "
                      "\"stateless\" (got \"%s\")",
                      cfg.name.c_str(), cfg.protocol.c_str());
            continue;
        }
        cfg.trusted = item.value("trusted", false);
        cfg.enabled = item.value("enabled", false);
        if (item.contains("timeout_ms") && item["timeout_ms"].is_number_integer())
            cfg.timeoutMs = item["timeout_ms"].get<int>();
        if (cfg.timeoutMs < 1000) cfg.timeoutMs = 1000;

        debugLogf("[mcp] server '%s' command=%s protocol=%s trusted=%d enabled=%d timeout=%dms",
                  cfg.name.c_str(), cfg.command.c_str(), cfg.protocol.c_str(), (int)cfg.trusted,
                  (int)cfg.enabled, cfg.timeoutMs);
        out.push_back(std::move(cfg));
    }
    debugLogf("[mcp] %zu server(s) loaded from '%s'", out.size(), path.c_str());
    return true;
}

bool saveEnabled(const std::string& path, const std::string& name, bool enabled, std::string& err) {
    err.clear();
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        err = "cannot open " + path;
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(ss.str());
    } catch (const std::exception& e) {
        err = std::string("invalid JSON: ") + e.what();
        return false;
    }
    if (!root.is_object() || !root.contains("servers") || !root["servers"].is_array()) {
        err = "expected an object with a \"servers\" array";
        return false;
    }
    bool found = false;
    for (auto& item : root["servers"]) {
        if (item.is_object() && item.value("name", std::string()) == name) {
            item["enabled"] = enabled;
            found = true;
            break;
        }
    }
    if (!found) {
        err = "no server named '" + name + "'";
        return false;
    }

    // Write through a temporary file so a crash mid-write cannot leave the user
    // with a truncated server list.
    const std::string tmp = path + ".tmp";
    {
        std::ofstream of(tmp, std::ios::binary | std::ios::trunc);
        if (!of) {
            err = "cannot write " + tmp;
            return false;
        }
        of << root.dump(2) << "\n";
        if (!of) {
            err = "write failed for " + tmp;
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        err = "could not replace " + path;
        return false;
    }
    debugLogf("[mcp] server '%s' enabled=%d written to '%s'", name.c_str(), (int)enabled,
              path.c_str());
    return true;
}

} // namespace mcp
