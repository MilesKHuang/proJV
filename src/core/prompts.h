#pragma once
#include <string>
#include <vector>

// Scan projv_prompts/ directory, create default .md files if missing.
// Returns list of .md filenames found in the directory (e.g. {"assistant.md", "reviewer.md"}).
// Presets: assistant.md (the one role seed), compactor.md (internal).
std::vector<std::string> ensureDefaultPrompts();

// Load a prompt file from projv_prompts/<filename>.
// Returns empty string if file does not exist or cannot be read.
std::string loadPromptFile(const std::string& filename);

// Upgrade a seeded prompt to the current default when it is still untouched
// (anchored, idempotent, returns the text unchanged when nothing applies).
// Exposed for unit tests; ensureDefaultPrompts() applies it on startup.
std::string upgradeSeededPrompt(const std::string& filename, std::string content);

// The role prompt seeded on first launch and selected at startup. One place, so
// a rename cannot leave a stale literal behind in the UI.
const char* defaultPromptFile();

// Index of defaultPromptFile() inside a role list: its own position when
// present, 0 for a non-empty list without it, -1 when the list is empty.
int defaultPromptIndex(const std::vector<std::string>& list);

// Seed text of defaultPromptFile(), exposed for unit tests.
std::string defaultPromptSeedText();
