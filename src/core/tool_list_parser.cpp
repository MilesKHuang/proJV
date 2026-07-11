#include "tool_list_parser.h"

std::vector<std::string> parseAvailableTools(const std::string& promptText) {
    static constexpr const char* TAG = "tools available:";
    auto pos = promptText.find(TAG);
    if (pos == std::string::npos) return {};

    pos += 16;  // skip "tools available:"
    while (pos < promptText.size() && promptText[pos] == ' ') ++pos;

    auto end = promptText.find('\n', pos);
    std::string line = promptText.substr(pos, end - pos);

    std::vector<std::string> tools;
    size_t start = 0;
    while (true) {
        auto comma = line.find(',', start);
        std::string name = line.substr(start, comma - start);
        auto first = name.find_first_not_of(" \t");
        auto last  = name.find_last_not_of(" \t");
        if (first != std::string::npos)
            tools.push_back(name.substr(first, last - first + 1));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return tools;
}
