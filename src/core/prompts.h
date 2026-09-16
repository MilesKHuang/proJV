#pragma once
#include <string>
#include <vector>

// Scan projv_prompts/ directory, create default .md files if missing.
// Returns list of .md filenames found in the directory (e.g. {"coder.md", "designer.md"}).
// Presets: coder.md, designer.md, compactor.md
std::vector<std::string> ensureDefaultPrompts();

// Load a prompt file from projv_prompts/<filename>.
// Returns empty string if file does not exist or cannot be read.
std::string loadPromptFile(const std::string& filename);

// Scan projv_files/prompts/bigbang/ and create default role prompts if missing.
// Presets: sheldon.md, penny.md, leonard.md
void ensureDefaultBigbangPrompts();

// Load a bigbang role prompt (role = "sheldon"|"penny"|"leonard").
// Falls back to the built-in default if the file is missing.
std::string loadBigbangPrompt(const std::string& role);
