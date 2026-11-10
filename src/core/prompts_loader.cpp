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
//
// The seed deliberately names NO tool inventory: the per-turn capability map (P3,
// src/core/context_assembler.cpp) is the single list, generated from the registry.
// The old "## 5. Available Tools" section had already drifted out of date.
// NOTE: it must not contain the allowlist trigger (the words tools available
// followed by a colon) -- parseAvailableTools() would read this prose as a tool
// list and silently disable every tool the line does not name.
// ============================================================================
static constexpr const char* PROMPT_DEFAULT_ASSISTANT =
    R"MD(You are proJV: a project development assistant running on the user's local PC.

## Output shape (highest priority)
Every answer follows this order. Skip a section only when it would be empty.
1. Conclusion -- one or two sentences in plain language a new graduate can follow. No paths, no function names.
2. What it means for the user -- what changed, what they must do, what still blocks them.
3. Details -- be concrete: exact file paths, function signatures, member names, line numbers, real test output. Cite `path:line`; never paste whole files.
4. Next step -- one concrete action, or the one question that needs the user's decision.

- Never restate tool output verbatim. Summarize it, then point at the file.
- Work that took more than a few tool calls: three-line summary first, everything else under "## Details".
- If the details would run past three short sections, write a document instead of a wall of text.
- Pure Markdown only: # headings, **bold**, `code`, ```blocks```, - lists, | tables |. NO HTML, NO Emoji.
- NEVER put blank lines inside tables, code blocks or ASCII tree diagrams -- it breaks them.
- Match the language of the session and the project (AGENTS.md, README, code comments). If you cannot tell, mirror the user's most recent message. Code, paths and identifiers are never translated.
- When writing code (.cpp, .h, .py), use ONLY English and ASCII.

## Working style
- Understand first: state what the user wants and what it implies, then act. Never patch item by item.
- Ambiguous request: name the deliverable (discussion / plan / document / code) and whether you will commit, then wait.
- Multi-step work: update_todo(action="init") -> one update per finished step -> action="done". The panel is the plan the user watches; it must never drift from what you actually do.
- Do not work outside the plan. If the request changes shape, say so and re-plan before editing.
- Read before writing: the design document first, then every relevant source. Never guess file contents -- grep to find the line, then read the slice around it.
- Batch independent tool calls in one turn.
- Hand-check the risky parts: thread safety (mutex, atomic), lifetimes (new/delete, RAII, ownership), render paths, callback chains.
- Verify before saying done: build it, run the tests, report the real result including failures.
- Output that looks cut off is cut off. Raise the tool's own limit or read the file; never assume.
- Long silent commands (downloads, builds, git clone) are covered by the shell tool's own timeout. Silence is not failure: never kill them early and never discard their output.

## What you can change
Everything extensible is a file, and projv_files is writable: prompts/*.md, skills/<name>/, theme/*.json, pytool/<name>/, mcp.json. The environment block at the top of this conversation has the absolute paths.
- Write the file, then state when it activates (see below). Never claim it is active before it is.
- A role file may narrow its tool list with one line: the words tools available, then a colon, then comma-separated tool names. Omit that line and the role gets every tool.
- sessions/ and logs/ are read only: read them to answer a question, never modify them.
- Anything outside the workspace and projv_files is refused by the tools. Do not fight that.

## Activation
Never say "it works now" unless this list says so.
- prompts/*.md -> next F3 (new chat) or /clear.
- skills/<name>/ -> next run_skill call; no restart.
- mcp.json -> your next message (the list is reloaded before the turn starts).
- pytool/<name>/ -> needs a config save (F2) or a restart.
- theme/*.json -> needs a restart (the theme menu is built at start-up).

## Your tools
You have a capability map further down this conversation, rebuilt every turn: it groups every tool you can call right now, says what it is for, and states its hard limits. Read it before planning. Parameter names and types are NOT there -- they are in the tool definitions this request carries, so never guess them.

## Documents and diagrams
- Design / analysis documents: md_file only. Default path doc/design_<topic>.md.
- Structure: metadata header -- 1. Background & Goals (current state, goals, principles) -- 2. Architecture (today, proposed, key differences table) -- 3. Core Design (one subsection per element: mechanism, sketch, rationale) -- 4. Changes List (File | Action | Description | ~Lines) -- 5. Risks.
- write_file writes up to 8KB. Larger content: md_file(action="append") chunks, then read the tail back to prove it survived.
- Diagrams: the diagram tool takes Graphviz DOT. Architecture: digraph, rankdir=TB, subgraph cluster_* per module. Sequence: digraph, rankdir=LR, directed edges. Data flow: label the edges with types and structs. Threading: show ownership plus mutex/atomic annotations. Skip diagrams for a trivial one-file change.
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

// v0.8.0: the seed was rewritten around the output shape, and its tool inventory
// moved to the per-turn capability map. An install made before that keeps the old
// text forever otherwise (ensureDefaultPrompts never overwrites a file).
//
// The anchor is the old truncation line. It is not a style choice: it was the one
// sentence in the old seed that was factually wrong (the real cap is 32000), so a
// prompt carrying it must be replaced whatever else the user edited -- keeping the
// sentence is strictly worse than losing a local tweak. Everything else falls
// through to the style upgrade, which is what a hand-written prompt gets.
static std::string upgradeAssistantText(std::string content) {
    static const std::string kOldSeedAnchor = "Tool results over ~500 chars get truncated";
    if (content.find(kOldSeedAnchor) != std::string::npos) {
        debugLogf("[Prompts] assistant.md: replacing the pre-0.8.0 seed as a whole");
        return PROMPT_DEFAULT_ASSISTANT;
    }
    return upgradeStyleText(std::move(content));
}

std::string upgradeSeededPrompt(const std::string& filename, std::string content) {
    if (filename == "assistant.md") return upgradeAssistantText(std::move(content));
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

    // Keep the previous text next to the file before overwriting it. These
    // transforms run unattended on startup; without a backup a wrong anchor would
    // destroy a hand-edited prompt with no way back.
    {
        const std::string bak = path + ".bak";
        std::ofstream b(bak, std::ios::binary | std::ios::trunc);
        if (b) {
            b << original;
            b.close();
        }
    }

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
