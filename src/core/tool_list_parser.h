#pragma once
#include <string>
#include <vector>

// Parse the "tools available:" line from a system prompt text.
// Returns the list of allowed tool names, or empty vector if no line found.
std::vector<std::string> parseAvailableTools(const std::string& promptText);
