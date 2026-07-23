#pragma once
#include <string>
#include <vector>

// ---- Prompt files (projv_files/prompts/) ----

// Scan projv_files/prompts/ directory, create default .md files if missing.
// Returns list of .md filenames found in the directory (e.g. {"coder.md", "designer.md"}).
// Presets: coder.md, designer.md, compactor.md, supervisor.md, analyzer.md, tester.md
std::vector<std::string> ensureDefaultPrompts();

// Load a prompt file from projv_files/prompts/<filename>.
// Returns empty string if file does not exist or cannot be read.
std::string loadPromptFile(const std::string& filename);

// ---- Workflow files (projv_files/workflows/) ----

// Scan projv_files/workflows/ directory, generate preset coding.json if missing.
// Returns list of .json filenames found in the directory (e.g. {"coding.json", "review.json"}).
std::vector<std::string> scanWorkflows(const std::string& configDir);

// Load a single workflow JSON file and return its parsed content.
// Returns empty string on failure.
std::string loadWorkflowJson(const std::string& jsonPath);

