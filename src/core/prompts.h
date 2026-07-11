#pragma once
#include <string>

// Load system_prompt.md from projv_prompts/ directory.
// Returns built-in default if file does not exist or cannot be read.
std::string loadSystemPrompt();

// Load compaction_prompt.md from projv_prompts/ directory.
// Returns built-in default if file does not exist or cannot be read.
std::string loadCompactionPrompt();
