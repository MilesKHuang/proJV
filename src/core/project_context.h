// proJV -- project context builder (shared by frontends).
//
// Builds a compact workspace directory-structure overview so the LLM knows
// where files actually live instead of guessing wrong paths.
#pragma once

#include <string>

std::string buildProjectContext(const std::string& workspacePath);
