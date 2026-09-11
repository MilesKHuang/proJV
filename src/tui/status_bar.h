// proJV TUI -- status bar text (1:1 with the legacy status bar).
#pragma once

#include "models.h"
#include "core/session.h"

#include <string>

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
};

// Render the one-line status bar.
std::string render(const Data& d);

} // namespace status_bar
