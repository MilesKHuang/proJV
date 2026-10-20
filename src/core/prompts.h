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

// Upgrade a seeded prompt to the current default when it is still untouched
// (anchored, idempotent, returns the text unchanged when nothing applies).
// Exposed for unit tests; ensureDefaultPrompts() applies it on startup.
std::string upgradeSeededPrompt(const std::string& filename, std::string content);
