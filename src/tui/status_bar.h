// proJV TUI -- status bar text (1:1 with the legacy status bar).
#pragma once

#include "models.h"
#include "core/session.h"
#include "theme_colors.h"

#include <string>
#include <vector>

namespace status_bar {

struct Data {
    std::string model;
    int promptTokens = 0;
    int completionTokens = 0;
    size_t msgCount = 0;
    int toolCount = 0;
    AgentStatus status;
    Session::PressureLevel pressure = Session::PressureLevel::Low;
    size_t estimatedTokens = 0;
    size_t windowTokens = 0;
    std::string workspace;
    std::string roleName;  // current system prompt role (without .md)
    double cost = 0.0;     // estimated cost in USD
};

// One colored segment of the status bar. `color` points at a ThemeColors field
// so every segment keeps the exact legacy status-bar color (never invented).
struct Segment {
    std::string text;
    std::string ThemeColors::* color;
};

// Render the status bar as colored segments (1:1 with the legacy status bar).
std::vector<Segment> renderSegments(const Data& d);

// Render the one-line status bar as plain text (kept for tests / debugging).
std::string render(const Data& d);

} // namespace status_bar
