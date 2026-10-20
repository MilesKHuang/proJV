#include "prompts.h"
#include "config.h"
#include "debug_log.h"
#include <fstream>
#include <filesystem>
#include <iterator>
#include <vector>
#include <algorithm>

// ============================================================================
// Built-in default coder prompt (used when coder.md doesn't exist)
// ============================================================================
static constexpr const char* PROMPT_DEFAULT_CODER =
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
    "- Get insight first: figure out what the user actually wants. NO ITEM-BY-ITEM PATCHING.\n"
    "- Confirm before acting when ambiguous: say what you understood + deliverable (discuss / plan / doc / code) + commit or not.\n"
    "- Don't rush to act: discuss or document first to minimize mistakes before executing.\n"
    "- Answer like a human: conclusion first, then what it means for the user, then the core underlying logic.\n"
    "- Put it in plain language -- make it as simple and easy to understand as possible, and reduce the user's cognitive cost while retaining the necessary professional details.\n"
    "\n"
    "## 2. WORKFLOW\n"
    "- Use `update_todo` for every multi-step task.\n"
    "- Read design docs before coding; generate diagrams for complex architecture.\n"
    "- Write tests after implementation; compile and verify before declaring done.\n"
    "\n"
    "## 3. Available Tools\n"
    "tools available: read_file, write_file, exec_shell, grep_files, edit_file, file_search, web_search, fetch_url, md_file, update_todo, diagram_tool, run_skill\n"
    "\n"
    "- `exec_shell` accepts an optional `timeout_ms` (number, milliseconds). Default is 300000 (5 min), max is 600000 (10 min). Long silent commands such as downloads, builds, and `git clone` are allowed until the total timeout; silence alone does NOT mean the command failed. Do not kill long-running commands early and do not discard all output to /dev/null.\n"
    "- `run_skill` starts an installed multi-agent skill (see the skill list in the workspace) and blocks until the whole workflow finishes -- it is expensive and slow (minutes). Use it only for explicitly complex multi-role tasks, never for something you can do with a normal tool.\n"
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
    "- Get insight first: understand what the user actually wants to design. NO ITEM-BY-ITEM PATCHING.\n"
    "- Confirm before acting when ambiguous: restate the goal + deliverable (discuss / plan / design doc) before writing.\n"
    "- Don't rush to write: read all relevant code and outline the design before drafting.\n"
    "- Answer like a human: conclusion first, then what it means for the user, then the core underlying logic.\n"
    "- Put it in plain language -- make it as simple and easy to understand as possible, and reduce the user's cognitive cost while retaining the necessary professional details.\n"
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
    "Produce a concise structured summary covering ONLY the key facts.\n"
    "\n"
    "## Hard Rules\n"
    "- Use EXACTLY the format below -- no extra sections, no commentary.\n"
    "- Keep the summary under 500 words.\n"
    "- Preserve exact file paths, error messages, and line numbers.\n"
    "- Do NOT fabricate information not present in the input.\n"
    "- A message header may carry a \"[+N images]\" marker. That means the user attached N images; keep the fact that those images were part of the conversation in your summary (never invent what they showed).\n"
    "\n"
    "### Goal\n[What the user is trying to accomplish]\n\n"
    "### Constraints\n[User's explicit restrictions or requirements]\n\n"
    "### Progress\n"
    "#### Done\n[Completed actions with file paths and key results]\n"
    "#### In Progress\n[Current work-in-progress]\n"
    "#### Blocked\n[What's stuck and why]\n\n"
    "### Key Decisions\n[Architectural choices, design trade-offs made]\n\n"
    "### Next step\n[The single next action -- one line]\n\n";

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
    "- Get insight first: understand what the user actually wants to analyze. NO ITEM-BY-ITEM PATCHING.\n"
    "- Confirm before acting when ambiguous: restate the goal + deliverable (discuss / plan / analysis doc + diagram) before writing.\n"
    "- Don't rush to write: scan and trace the data flow before drafting.\n"
    "- Answer like a human: conclusion first, then what it means for the user, then the core underlying logic.\n"
    "- Put it in plain language -- make it as simple and easy to understand as possible, and reduce the user's cognitive cost while retaining the necessary professional details.\n"
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
// Helper: get the projv_prompts directory path
// ============================================================================
static std::string promptsDir() {
    return getPromptsDir();
}

// ============================================================================
// Seeds upgrade - pure text transforms, unit-tested in tests/unit
// ----------------------------------------------------------------------------
// ensureDefaultPrompts() never overwrites an existing prompt, so installs made
// before a feature landed keep the old seeded text. Each transform below is
// anchored on a line only the seeded default carries and is idempotent, so a
// hand-edited prompt is never touched.
// ============================================================================
static const char* kCoderToolsOld =
    "tools available: read_file, write_file, exec_shell, grep_files, edit_file, "
    "file_search, web_search, fetch_url, md_file, update_todo, diagram_tool";

static std::string upgradeCoderText(std::string content) {
    const std::string oldLine = kCoderToolsOld;
    const size_t pos = content.find(oldLine);
    if (pos == std::string::npos) return content;

    // The upgraded line contains the old one as a prefix -- require a line end
    // right after the match so the upgrade stays idempotent.
    const size_t after = pos + oldLine.size();
    if (after < content.size()) {
        const char c = content[after];
        if (c != '\r' && c != '\n') return content;
    }
    content.insert(after, ", run_skill");
    return content;
}

static std::string upgradeCompactorText(std::string content) {
    static const std::string kAnchor =
        "- Do NOT fabricate information not present in the input.";
    static const std::string kMarker = "[+N images]";
    static const std::string kLine =
        "\n- A message header may carry a \"[+N images]\" marker: the user attached N "
        "images to that message. Keep the fact that those images were part of the "
        "conversation in your summary, and never invent what they showed.";

    if (content.find(kMarker) != std::string::npos) return content;   // already done
    const size_t pos = content.find(kAnchor);
    if (pos == std::string::npos) return content;                     // rewritten
    content.insert(pos + kAnchor.size(), kLine);
    return content;
}

// Communication style: the seeds used to carry a terser one-liner. It is
// rewritten into the two-line rule (structure + plain language) that all three
// role prompts now share.
static const char* kStyleOldYou =
    "- Answer like a human: conclusion first, then what it means for you; "
    "then expose the core underlying logic.";
static const char* kStyleOldUser =
    "- Answer like a human: conclusion first, then what it means for the user; "
    "then expose the core logic.";
static const char* kStyleOldBare =
    "- Answer like a human: conclusion first, then what it means; "
    "then expose the core logic.";
static const char* kStyleNew =
    "- Answer like a human: conclusion first, then what it means for the user, "
    "then the core underlying logic.\n"
    "- Put it in plain language -- make it as simple and easy to understand as "
    "possible, and reduce the user's cognitive cost while retaining the necessary "
    "professional details.";

static std::string upgradeStyleText(std::string content) {
    // Idempotent: the second line only exists after the upgrade.
    if (content.find("Put it in plain language") != std::string::npos)
        return content;

    static const std::string olds[] = {kStyleOldYou, kStyleOldUser, kStyleOldBare};
    for (const auto& oldLine : olds) {
        const size_t pos = content.find(oldLine);
        if (pos == std::string::npos) continue;
        content.replace(pos, oldLine.size(), kStyleNew);
        break;
    }
    return content;
}

std::string upgradeSeededPrompt(const std::string& filename, std::string content) {
    if (filename == "coder.md") {
        content = upgradeStyleText(std::move(content));
        return upgradeCoderText(std::move(content));
    }
    if (filename == "designer.md" || filename == "analyzer.md")
        return upgradeStyleText(std::move(content));
    if (filename == "compactor.md") return upgradeCompactorText(std::move(content));
    return content;
}

// Apply upgradeSeededPrompt() to a file on disk (no-op when unchanged).
static void upgradePromptFile(const std::string& filename, const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs) return;
    const std::string original((std::istreambuf_iterator<char>(ifs)), {});
    ifs.close();

    std::string upgraded = upgradeSeededPrompt(filename, original);
    if (upgraded == original) return;

    std::ofstream ofs(path, std::ios::trunc);
    if (!ofs) return;
    ofs << upgraded;
    debugLogf("[Prompts] Upgraded %s", filename.c_str());
}

// ============================================================================
// ensureDefaultPrompts - scan projv_prompts/*.md, create presets if missing
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
        {"analyzer.md",   PROMPT_DEFAULT_ANALYZER},
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

    // Upgrade stale seeded prompts in place (see upgradeSeededPrompt above).
    upgradePromptFile("coder.md", dir + "/coder.md");
    upgradePromptFile("designer.md", dir + "/designer.md");
    upgradePromptFile("analyzer.md", dir + "/analyzer.md");
    upgradePromptFile("compactor.md", dir + "/compactor.md");

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
