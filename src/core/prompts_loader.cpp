#include "prompts.h"
#include "config.h"
#include "debug_log.h"
#include <fstream>
#include <filesystem>
#include <iterator>
#include <vector>
#include <algorithm>

// ============================================================================
// Built-in default assistant prompt -- the one role seed.
// One file for every kind of work (code, design, analysis, writing): the plan
// lives in the TODO list, not in a second prompt file. ASCII only.
// ============================================================================
static constexpr const char* PROMPT_DEFAULT_ASSISTANT =
    R"MD(You are a project development assistant running on the user's local PC. You have FULL access to all tools: read code, write code, search the web, design architecture, generate diagrams, run tests, and execute shell commands.

## 1. Hard Rules
- Pure Markdown only: # headings, **bold**, `code`, ```blocks```, - lists, | tables |. NO HTML, NO Emoji.
- Lead with the conclusion in language a new graduate can follow: first what it is and what it means for the user, then the professional details (paths, signatures, data, parameters).
- Match the language of the session and the project -- the language most of AGENTS.md, README and the code comments use. If you cannot tell, mirror the user's most recent message. Code, file paths and function names stay exactly as they are; never translate them.
- Be concrete -- exact file paths, function signatures, member names, and estimated line counts.
- When writing code (.cpp, .h, .py), use ONLY English and ASCII.
- write_file: up to 8KB. Larger files -> exec_shell with stdin.
- NEVER put blank lines inside tables, code blocks, or ASCII tree diagrams -- it breaks the formatting.
- Tool results over ~500 chars get truncated. Use exec_shell for full output.
- Batch independent tool calls (reads/writes) in the same turn.
- Get insight first: figure out what the user actually wants. NO ITEM-BY-ITEM PATCHING.
- Confirm before acting when ambiguous: state what you understood and what it means for the user, name the deliverable (discuss / plan / doc / code) and whether you will commit, then wait.
- Don't rush to act: discuss or document first to minimize mistakes before executing.

## 2. Plan And Execute
- Plan first: a multi-step task starts with update_todo(action="init", tasks="- one per line").
- Execute step by step: update_todo(action="update", task="<task>", status="in_progress" or "done", brief="<file>: <change> [L<line>] - <result>").
- Close with update_todo(action="done"). The TODO panel is the plan the user watches; it must never drift from what you actually do.
- Do not do work outside the plan. If the request changes shape, say so and re-plan before editing anything.
- Read design docs before coding. Read ALL relevant sources before writing -- never guess file contents.
- Hand-check the risky parts: thread safety (mutex, atomic), resource lifecycle (new/delete, RAII), render paths, callback chains, pointer ownership.
- Write tests after implementation; compile and verify before declaring done.

## 3. Documents
- Design and analysis documents go through md_file ONLY. Default path: doc/design_<topic>.md.
- Structure: metadata header -- 1. Background & Goals (current state, goals, principles) -- 2. Architecture (today, proposed, key differences table) -- 3. Core Design (one subsection per element: mechanism, sketch, rationale) -- 4. Changes List (File | Action | Description | ~Lines) -- 5. Risks & Considerations.
- Over 8KB: continue with md_file(action="append") chunks, then read the tail back to prove it survived.

## 4. Diagrams
- Architecture: digraph, rankdir=TB, subgraph cluster_* per module.
- Sequence: digraph, rankdir=LR, directed edges.
- Data flow: label the edges with types and structs.
- Threading: show ownership plus mutex/atomic annotations.
- Skip diagrams for trivial single-file changes.

## 5. Available Tools
tools available: read_file, write_file, exec_shell, grep_files, edit_file, file_search, web_search, fetch_url, md_file, update_todo, diagram_tool, run_skill, view_image, list_dir, format_file

- `exec_shell` accepts an optional `timeout_ms` (number, milliseconds). Default is 1800000 (30 min), which is also the hard cap; pass a smaller value only to fail faster. Long silent commands such as downloads, builds, and `git clone` are allowed until the total timeout; silence alone does NOT mean the command failed. Do not kill long-running commands early and do not discard all output to /dev/null.
- `run_skill` starts an installed multi-agent skill (see the skill list in the workspace) and blocks until the whole workflow finishes -- it is expensive and slow (minutes). Use it only for explicitly complex multi-role tasks, never for something you can do with a normal tool.
- `view_image` lets you look at images on disk (Flash models only). The images arrive as a user message in the NEXT turn, so call it on its own: it ends the current turn. Never batch it with edits, shell commands or `run_skill`.
- `list_dir` lists a directory tree to a given depth -- use it when planning instead of guessing paths.
- `format_file` runs clang-format (or a whitespace pass) on one file -- use it after a large edit instead of hand-aligning.
)MD";

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
    "- A message header may carry a \"[+N images]\" marker. That means the user attached N images; "
    "keep the fact that those images were part of the conversation in your summary (never invent "
    "what they showed).\n"
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
    if (pos != std::string::npos) {
        // The upgraded line contains the old one as a prefix -- require a line end
        // right after the match so the upgrade stays idempotent.
        const size_t after = pos + oldLine.size();
        if (after >= content.size() || content[after] == '\r' || content[after] == '\n')
            content.insert(after, ", run_skill");
    }
    // v0.6.3: view_image joined the tool line. Anchored on a "run_skill" that
    // ends its line, so a hand-edited prompt whose tool list stops elsewhere is
    // left alone, and a second pass does nothing.
    {
        static const std::string kDone = "run_skill, view_image";
        if (content.find(kDone) == std::string::npos) {
            static const std::string kAnchor = "run_skill";
            size_t p = content.find(kAnchor);
            while (p != std::string::npos) {
                const size_t after = p + kAnchor.size();
                if (after >= content.size() || content[after] == '\r' || content[after] == '\n') {
                    content.insert(after, ", view_image");
                    break;
                }
                p = content.find(kAnchor, after);
            }
        }
    }
    return content;
}

static std::string upgradeCompactorText(std::string content) {
    static const std::string kAnchor = "- Do NOT fabricate information not present in the input.";
    static const std::string kMarker = "[+N images]";
    static const std::string kLine =
        "\n- A message header may carry a \"[+N images]\" marker: the user attached N "
        "images to that message. Keep the fact that those images were part of the "
        "conversation in your summary, and never invent what they showed.";

    if (content.find(kMarker) != std::string::npos) return content; // already done
    const size_t pos = content.find(kAnchor);
    if (pos == std::string::npos) return content; // rewritten
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
static const char* kStyleOldBare = "- Answer like a human: conclusion first, then what it means; "
                                   "then expose the core logic.";
static const char* kStyleNew =
    "- Answer like a human: conclusion first, then what it means for the user, "
    "then the core underlying logic.\n"
    "- Put it in plain language -- make it as simple and easy to understand as "
    "possible, and reduce the user's cognitive cost while retaining the necessary "
    "professional details.";

static std::string upgradeStyleText(std::string content) {
    // Idempotent: the second line only exists after the upgrade.
    if (content.find("Put it in plain language") != std::string::npos) return content;

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
    if (filename == "assistant.md") return upgradeStyleText(std::move(content));
    // Legacy role seeds. They are no longer written and no longer listed as
    // roles, but installs made before v0.6.6 still have the files on disk and a
    // user may copy one back, so the branches stay.
    if (filename == "coder.md") {
        content = upgradeStyleText(std::move(content));
        return upgradeCoderText(std::move(content));
    }
    if (filename == "designer.md" || filename == "analyzer.md")
        return upgradeStyleText(std::move(content));
    if (filename == "compactor.md") return upgradeCompactorText(std::move(content));
    return content;
}

const char* defaultPromptFile() {
    return "assistant.md";
}

int defaultPromptIndex(const std::vector<std::string>& list) {
    if (list.empty()) return -1;
    const std::string want = defaultPromptFile();
    for (size_t i = 0; i < list.size(); ++i)
        if (list[i] == want) return static_cast<int>(i);
    return 0; // the file is gone but the list is not: first role wins
}

std::string defaultPromptSeedText() {
    return PROMPT_DEFAULT_ASSISTANT;
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
        {defaultPromptFile(), PROMPT_DEFAULT_ASSISTANT},
        {"compactor.md", COMPACTOR_PROMPT_DEFAULT},
    };

    for (auto& p : presets) {
        std::string path = dir + "/" + p.filename;
        if (!std::filesystem::exists(path)) {
            std::ofstream ofs(path);
            if (ofs) {
                ofs << p.content;
                debugLogf("[Prompts] Created %s from built-in default (%zu chars)", p.filename,
                          strlen(p.content));
            }
        }
    }

    // Upgrade stale seeded prompts in place (see upgradeSeededPrompt above).
    upgradePromptFile(defaultPromptFile(), dir + "/" + defaultPromptFile());
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
                if (ext == ".md") files.push_back(name);
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
