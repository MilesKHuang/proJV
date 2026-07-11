#include "prompts.h"
#include "config.h"
#include "debug_log.h"
#include <fstream>
#include <filesystem>

// ============================================================================
// Built-in default system prompt (used when system_prompt.md doesn't exist)
// ============================================================================
static constexpr const char* PROMPT_DEFAULT_SYSTEM =
    "You are a coding assistant running on the user's local PC. "
    "Execute shell commands and read/write files to help with development tasks.\n"
    "\n"
    "## 1. Hard Rules\n"
    "- Pure Markdown only: # headings, **bold**, `code`, ```blocks```, - lists, | tables |. NO HTML, NO Emoji.\n"
    "- Keep responses concise -- compact conclusions, not essay paragraphs.\n"
    "- Batch independent tool calls (reads/writes) in the same turn.\n"
    "- When writing code (.cpp, .h, .py), use ONLY English and ASCII.\n"
    "- write_file: up to 8KB. Larger files -> exec_shell with stdin.\n"
    "- Tool results over ~500 chars get truncated. Use exec_shell for full output.\n"
    "\n"
    "## 2. WORKFLOW -- use `update_todo` for every multi-step task\n"
    "- **INIT:** Parse request -> list all sub-tasks with file paths -> `update_todo(action=\"init\", tasks=\"...\")`\n"
    "- **EXECUTE:** Work through tasks one by one. After EACH: `update_todo(action=\"update\", task=\"...\", status=\"done\", brief=\"<file>: <change> [L<line>] -- <result>\")`\n"
    "- **VERIFY:** Read back changed files, confirm edits landed. Errors -> fix -> re-update.\n"
    "- **DONE:** `update_todo(action=\"done\")` -- then output final answer.\n"
    "\n"
    "## 3. Available Tools\n"
    "tools available: read_file, write_file, exec_shell, grep_files, edit_file, file_search, web_search, fetch_url, update_todo\n"
    "\n";

// ============================================================================
// Built-in default compaction prompt (used when compaction_prompt.md doesn't exist)
// ============================================================================
static constexpr const char* COMPACTION_DEFAULT =
    "You are a context-compaction assistant. Below are older messages from a coding session. "
    "Produce a concise structured summary covering ONLY the key facts. "
    "Use EXACTLY this format:\n\n"
    "### Goal\n[What the user is trying to accomplish]\n\n"
    "### Constraints\n[User's explicit restrictions or requirements]\n\n"
    "### Progress\n"
    "#### Done\n[Completed actions with file paths and key results]\n"
    "#### In Progress\n[Current work-in-progress]\n"
    "#### Blocked\n[What's stuck and why]\n\n"
    "### Key Decisions\n[Architectural choices, design trade-offs made]\n\n"
    "### Next step\n[The single next action -- one line]\n\n"
    "Keep the summary under 500 words. Preserve exact file paths, error messages, "
    "and line numbers. Do NOT fabricate information not present in the input.";

// ============================================================================
// Helper: get the projv_prompts directory path
// ============================================================================
static std::string promptsDir() {
    std::string configPath = getConfigPath();
    auto parent = std::filesystem::path(configPath).parent_path();
    return (parent / "projv_prompts").string();
}

// ============================================================================
// Load a prompt file, return its content. Returns empty string on failure.
// ============================================================================
static std::string loadPromptFile(const std::string& filename) {
    std::string path = promptsDir() + "/" + filename;
    std::ifstream ifs(path);
    if (!ifs) {
        debugLogf("[Prompts] File not found: %s", path.c_str());
        return {};
    }
    std::string content((std::istreambuf_iterator<char>(ifs)), {});
    if (content.empty()) {
        debugLogf("[Prompts] File empty: %s", path.c_str());
    } else {
        debugLogf("[Prompts] Loaded %s (%zu chars)", path.c_str(), content.size());
    }
    return content;
}

// ============================================================================
// Public API
// ============================================================================

std::string loadSystemPrompt() {
    std::string content = loadPromptFile("system_prompt.md");
    if (!content.empty()) return content;

    // Fallback: auto-create system_prompt.md from built-in default
    std::string path = promptsDir() + "/system_prompt.md";
    std::filesystem::create_directories(promptsDir());
    std::string fallback = PROMPT_DEFAULT_SYSTEM;
    {
        std::ofstream ofs(path);
        if (ofs) {
            ofs << fallback;
            debugLogf("[Prompts] Created system_prompt.md from built-in default (%zu chars)", fallback.size());
        }
    }
    return fallback;
}

std::string loadCompactionPrompt() {
    std::string content = loadPromptFile("compaction_prompt.md");
    if (!content.empty()) return content;

    // Fallback: auto-create compaction_prompt.md from built-in default
    std::string path = promptsDir() + "/compaction_prompt.md";
    std::filesystem::create_directories(promptsDir());
    std::string fallback = COMPACTION_DEFAULT;
    {
        std::ofstream ofs(path);
        if (ofs) {
            ofs << fallback;
            debugLogf("[Prompts] Created compaction_prompt.md from built-in default (%zu chars)", fallback.size());
        }
    }
    return fallback;
}
