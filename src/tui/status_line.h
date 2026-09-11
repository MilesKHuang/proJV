// proJV TUI -- status line text (1:1 with the input-area status line).
#pragma once

#include "models.h"

#include <string>

namespace status_line {

// Render the one-line agent status text for the given snapshot.
std::string render(const AgentStatus& status);

} // namespace status_line
