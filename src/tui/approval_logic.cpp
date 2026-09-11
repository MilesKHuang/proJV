// proJV TUI -- approval dialog logic implementation.
#include "approval_logic.h"

#include "json.hpp"

#include <cctype>
#include <cstring>
#include <string>
#include <vector>

namespace approval_logic {

std::string extractCommand(const std::string& argumentsJson) {
    try {
        std::string cmd = nlohmann::json::parse(argumentsJson).value("command", "");
        return cmd;
    } catch (...) {
        return argumentsJson;
    }
}

std::vector<std::string> extractDeleteFiles(const std::string& command) {
    std::vector<std::string> files;

    std::string clower = command;
    for (auto& c : clower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    size_t cmdPos = std::string::npos;
    const char* deleteKws[] = {"del ", "erase ", "rm ", "rmdir ", "rd "};
    for (const auto& kw : deleteKws) {
        size_t pos = clower.find(kw);
        while (pos != std::string::npos) {
            bool ok = (pos == 0) || (clower[pos - 1] == ' ' || clower[pos - 1] == '\t');
            if (ok) {
                cmdPos = pos + strlen(kw);
                break;
            }
            pos = clower.find(kw, pos + 1);
        }
        if (cmdPos != std::string::npos) break;
    }

    if (cmdPos == std::string::npos) return files;

    std::string args = command.substr(cmdPos);
    std::string current;
    for (size_t i = 0; i < args.size(); ++i) {
        char c = args[i];
        if (c == ' ') {
            if (!current.empty()) {
                if (current[0] != '/' && current[0] != '-') files.push_back(current);
                current.clear();
            }
        } else if (c == '"') {
            ++i;
            while (i < args.size() && args[i] != '"') current += args[i++];
            if (!current.empty()) { files.push_back(current); current.clear(); }
        } else {
            current += c;
        }
    }
    if (!current.empty() && current[0] != '/' && current[0] != '-') {
        files.push_back(current);
    }

    return files;
}

} // namespace approval_logic
