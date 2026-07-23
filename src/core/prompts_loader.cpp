#include "prompts.h"
#include "config.h"
#include "debug_log.h"
#include <fstream>
#include <filesystem>
#include <vector>
#include <algorithm>

// ============================================================================
// Built-in default coder prompt (used when coder.md doesn't exist)
// ============================================================================
static constexpr const char* PROMPT_DEFAULT_CODER =
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
    "tools available: read_file, write_file, exec_shell, grep_files, edit_file, file_search, update_todo\n"
    "\n";

// ============================================================================
// Built-in default designer prompt (used when designer.md doesn't exist)
// ============================================================================
static constexpr const char* DESIGNER_PROMPT_DEFAULT =
    "You are a design assistant. Your job is to read project source code thoroughly, "
    "understand the architecture, and produce a structured, actionable design document.\n"
    "\n"
    "## 1. Hard Rules\n"
    "- Pure Markdown: # headings, **bold**, `code`, ```blocks```, - lists, | tables |. NO HTML, NO Emoji.\n"
    "- Chinese for narrative; English for code, file paths, function names, and technical terms.\n"
    "- Be concrete -- include exact file paths, function signatures, member names, and estimated line counts.\n"
    "- NEVER put blank lines inside tables, code blocks, or ASCII tree diagrams -- it breaks formatting.\n"
    "- Keep responses concise -- compact conclusions, not essay paragraphs.\n"
    "- Write design documents using **md_file** tool ONLY.\n"
    "- You CANNOT execute shell commands or modify source code files (.cpp, .h, .py, etc.).\n"
    "- Batch independent reads in the same turn.\n"
    "\n"
    "## 2. Available Tools\n"
    "tools available: md_file, read_file, grep_files\n"
    "\n"
    "## 3. Document Structure\n"
    "Output to `doc/design_<topic>.md` under the workspace root.\n"
    "The document must contain these sections:\n"
    "```\n"
    "Metadata header -- version, date, status (Design Phase).\n"
    "\n"
    "1. Background & Goals\n"
    "- 1.1 Current State: what exists today -- files, functions, pain points.\n"
    "- 1.2 Goals: numbered list of concrete outcomes.\n"
    "- 1.3 Design Principles: bullet list of guiding principles.\n"
    "\n"
    "2. Architecture\n"
    "- 2.1 Current Architecture: ASCII tree showing relevant files and call relationships.\n"
    "- 2.2 Proposed Architecture: same tree with new/modified/deleted markers.\n"
    "- 2.3 Key Differences: table comparing Component | Current | Proposed.\n"
    "\n"
    "3. Core Design\n"
    "- One subsection per major design element (3.1, 3.2, ...).\n"
    "- Each: describe mechanism, show code/interface sketch, explain rationale.\n"
    "\n"
    "4. Changes List\n"
    "- Table: File | Action (New/Modified/Deleted) | Description | ~Lines.\n"
    "- Include every file that must change.\n"
    "\n"
    "5. Risks & Considerations\n"
    "- One subsection per risk: what could go wrong, why it matters, mitigation.\n"
    "```\n"
    "## 4. Workflow\n"
    "1. Read ALL relevant source files with `read_file` -- do not guess file contents.\n"
    "2. Use `grep_files` to trace callers/callees of key functions and understand data flow.\n"
    "3. Pay special attention to: thread safety (mutex, atomic), resource lifecycle (new/delete, RAII), render paths, callback chains, and pointer ownership.\n"
    "4. Draft the design document with `md_file(action=\"write\", ...)`.\n"
    "5. If the document exceeds 8KB, split into multiple `md_file` calls using `edit` to append.\n"
    "6. Stop and wait for user review. Do NOT proceed to implementation.\n"
    "\n";

// ============================================================================
// Built-in default compactor prompt (used when compactor.md doesn't exist)
// ============================================================================
static constexpr const char* COMPACTOR_PROMPT_DEFAULT =
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
// Built-in default supervisor prompt (used when supervisor.md doesn't exist)
// ============================================================================
static constexpr const char* PROMPT_DEFAULT_SUPERVISOR =
    "You are a project development assistant running on the user's local PC. "
    "You have FULL access to all tools: read code, write code, search the web, "
    "design architecture, generate diagrams, run tests, and execute shell commands.\n"
    "\n"
    "## 1. Hard Rules\n"
    "- Pure Markdown only: # headings, **bold**, `code`, ```blocks```, - lists, | tables |. NO HTML, NO Emoji.\n"
    "- Keep responses concise -- compact conclusions, not essay paragraphs.\n"
    "- Batch independent tool calls (reads/writes) in the same turn.\n"
    "- When writing code (.cpp, .h, .py), use ONLY English and ASCII.\n"
    "- write_file: up to 8KB. Larger files -> exec_shell with stdin.\n"
    "- Tool results over ~500 chars get truncated. Use exec_shell for full output.\n"
    "\n"
    "## 2. WORKFLOW\n"
    "- Use `update_todo` for every multi-step task.\n"
    "- Read design docs before coding; generate diagrams for complex architecture.\n"
    "- Write tests after implementation; compile and verify before declaring done.\n"
    "\n"
    "## 3. Available Tools\n"
    "tools available: read_file, write_file, exec_shell, grep_files, edit_file, file_search, web_search, fetch_url, md_file, update_todo, diagram_tool\n"
    "\n";

// ============================================================================
// Built-in default analyzer prompt (used when analyzer.md doesn't exist)
// ============================================================================
static constexpr const char* PROMPT_DEFAULT_ANALYZER =
    "You are a project analysis assistant. Thoroughly read source code, trace data flow, "
    "and produce analysis documents with supporting diagrams.\n"
    "\n"
    "## 1. Hard Rules\n"
    "- Pure Markdown: # headings, **bold**, `code`, ```blocks```, - lists, | tables |. NO HTML, NO Emoji.\n"
    "- Chinese for narrative; English for code, file paths, function names.\n"
    "- Be concrete: exact file paths, function signatures, line counts.\n"
    "- You CANNOT modify source files -- analysis ONLY.\n"
    "- Batch independent reads in the same turn.\n"
    "\n"
    "## 2. Workflow\n"
    "1. **Scan:** `file_search` to discover files and directories.\n"
    "2. **Deep read:** `read_file` on all key sources. Do not guess.\n"
    "3. **Trace:** `grep_files` for callers/callees and data flow.\n"
    "4. **Visualize:** `diagram_tool` for architecture/sequence/flow diagrams. "
    "Skip trivial single-file modules.\n"
    "5. **Document:** `md_file` to doc/architecture.md.\n"
    "\n"
    "## 3. Diagram Guidelines\n"
    "- Architecture: `digraph`, `rankdir=TB`, `subgraph cluster_*` for modules.\n"
    "- Sequence: `digraph`, `rankdir=LR`, directed edges.\n"
    "- Data flow: label edges with types/structs.\n"
    "- Threading: show ownership, mutex/atomic annotations.\n"
    "\n"
    "## 4. Available Tools\n"
    "tools available: read_file, grep_files, file_search, md_file, diagram_tool\n"
    "\n";

// ============================================================================
// Built-in default tester prompt (used when tester.md doesn't exist)
// ============================================================================
static constexpr const char* PROMPT_DEFAULT_TESTER =
    "You are a testing assistant. Your job is to write test code, compile, "
    "execute tests, and report results clearly.\n"
    "\n"
    "## 1. Hard Rules\n"
    "- Pure Markdown only. NO HTML, NO Emoji.\n"
    "- When writing test code, use ONLY English and ASCII.\n"
    "- Always compile and run tests after writing them.\n"
    "- Report PASS/FAIL clearly with exact error messages and line numbers.\n"
    "- If tests fail, analyze the root cause and suggest fixes -- but do NOT modify source code.\n"
    "- Batch independent reads in the same turn.\n"
    "\n"
    "## 2. Workflow\n"
    "1. **Read:** `read_file` on the target source code to understand what to test.\n"
    "2. **Write tests:** `write_file` for new test files, covering edge cases and normal paths.\n"
    "3. **Compile:** `exec_shell` to build the test target.\n"
    "4. **Run:** `exec_shell` to execute tests, capture output.\n"
    "5. **Report:** Output a structured test report with test file, PASS/FAIL counts, "
    "specific failure details (test name, error message, line number), and coverage notes.\n"
    "\n"
    "## 3. Available Tools\n"
    "tools available: read_file, write_file, exec_shell, grep_files, file_search, update_todo\n"
    "\n";

// ============================================================================
// Helper: get the projv_files/prompts directory path
// ============================================================================
static std::string promptsDir() {
    std::string configPath = getConfigPath();
    auto parent = std::filesystem::path(configPath).parent_path();
    return (parent / "projv_files/prompts").string();
}

// ============================================================================
// ensureDefaultPrompts - scan projv_files/prompts/*.md, create presets if missing
// ============================================================================
std::vector<std::string> ensureDefaultPrompts() {
    std::string dir = promptsDir();
    std::filesystem::create_directories(dir);

    // Preset definitions: filename -> default content
    struct Preset {
        const char* filename;
        const char* content;
    };
    const Preset presets[] = {
        {"coder.md",      PROMPT_DEFAULT_CODER},
        {"designer.md",   DESIGNER_PROMPT_DEFAULT},
        {"compactor.md",  COMPACTOR_PROMPT_DEFAULT},
        {"supervisor.md", PROMPT_DEFAULT_SUPERVISOR},
        {"analyzer.md",   PROMPT_DEFAULT_ANALYZER},
        {"tester.md",     PROMPT_DEFAULT_TESTER},
    };

    for (auto& p : presets) {
        std::string path = dir + "/" + p.filename;
        if (!std::filesystem::exists(path)) {
            std::ofstream ofs(path);
            if (ofs) {
                ofs << p.content;
                debugLogf("[Prompts] Created %s from built-in default (%zu chars)",
                    p.filename, strlen(p.content));
            }
        }
    }

    // Scan all .md files in the directory
    std::vector<std::string> files;
    std::error_code ec;
    for (auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (entry.is_regular_file()) {
            std::string name = entry.path().filename().string();
            if (name.size() >= 3) {
                std::string ext = name.substr(name.size() - 3);
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext == ".md")
                    files.push_back(name);
            }
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

// ============================================================================
// loadPromptFile - load a prompt file by filename
// ============================================================================
std::string loadPromptFile(const std::string& filename) {
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
// Workflow file scanning (projv_files/workflows/)
// ============================================================================

// Helper: get projv_files/workflows directory path
static std::string workflowsDir() {
    std::string configPath = getConfigPath();
    auto parent = std::filesystem::path(configPath).parent_path();
    return (parent / "projv_files/workflows").string();
}

// Built-in preset coding.json content
static constexpr const char* PRESET_CODING_JSON =
R"({
  "version": "1.0",
  "description": "Default coding workflow -- full development cycle with analysis, design, implementation, and testing",

  "main_agent": {
    "prompt_file": "supervisor.md",
    "model": ""
  },

  "subagents": [
    {
      "name": "coder",
      "display": "Coder",
      "prompt_file": "coder.md",
      "description": "Code implementation, compilation, debugging",
      "model": "deepseek-v4-flash",
      "visible": true
    },
    {
      "name": "designer",
      "display": "Designer",
      "prompt_file": "designer.md",
      "description": "Architecture design documents",
      "model": "deepseek-v4-flash",
      "visible": true
    },
    {
      "name": "analyzer",
      "display": "Analyzer",
      "prompt_file": "analyzer.md",
      "description": "Code analysis, call tracing, diagram generation",
      "model": "deepseek-v4-flash",
      "visible": true
    },
    {
      "name": "tester",
      "display": "Tester",
      "prompt_file": "tester.md",
      "description": "Test writing, compilation, execution, reporting",
      "model": "deepseek-v4-flash",
      "visible": true
    }
  ]
})";

std::vector<std::string> scanWorkflows(const std::string& configDir) {
    namespace fs = std::filesystem;
    std::string dir = configDir + "/projv_files/workflows";
    std::error_code ec;
    bool dirExists = fs::is_directory(dir, ec);

    if (!dirExists || ec) {
        // Create directory and generate preset
        fs::create_directories(dir, ec);
        if (ec) {
            debugLogf("[Workflows] Failed to create dir: %s", dir.c_str());
            return {};
        }
    }

    // Check if directory has any .json files
    std::vector<std::string> files;
    for (auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (entry.is_regular_file()) {
            std::string name = entry.path().filename().string();
            if (name.size() >= 5) {
                std::string ext = name.substr(name.size() - 5);
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext == ".json")
                    files.push_back(name);
            }
        }
    }

    if (files.empty()) {
        // Generate preset coding.json
        std::string presetPath = dir + "/coding.json";
        std::ofstream ofs(presetPath);
        if (ofs) {
            ofs << PRESET_CODING_JSON;
            files.push_back("coding.json");
            debugLogf("[Workflows] Generated preset: %s", presetPath.c_str());
        } else {
            debugLogf("[Workflows] Failed to create preset: %s", presetPath.c_str());
        }
    }

    std::sort(files.begin(), files.end());
    return files;
}

std::string loadWorkflowJson(const std::string& jsonPath) {
    std::ifstream ifs(jsonPath);
    if (!ifs) {
        debugLogf("[Workflows] Cannot open: %s", jsonPath.c_str());
        return {};
    }
    std::string content((std::istreambuf_iterator<char>(ifs)), {});
    return content;
}
