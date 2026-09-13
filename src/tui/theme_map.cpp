// proJV TUI -- theme color mapping implementation.
#include "theme_map.h"

#include <string>

namespace theme_map {

ftxui::Color hexToColor(const std::string& hex) {
    if (hex.size() < 7 || hex[0] != '#') {
        return ftxui::Color::RGB(255, 255, 255);
    }
    unsigned int r, g, b;
    try {
        r = std::stoul(hex.substr(1, 2), nullptr, 16);
        g = std::stoul(hex.substr(3, 2), nullptr, 16);
        b = std::stoul(hex.substr(5, 2), nullptr, 16);
    } catch (...) {
        return ftxui::Color::RGB(255, 255, 255);
    }
    return ftxui::Color::RGB(
        static_cast<uint8_t>(r),
        static_cast<uint8_t>(g),
        static_cast<uint8_t>(b));
}

} // namespace theme_map
