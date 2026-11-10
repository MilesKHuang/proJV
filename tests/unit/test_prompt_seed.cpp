// Tests for the single universal role seed (assistant.md).
//
// The seed is the product: if it loses a hard rule, every session degrades and
// nothing else goes red. So the invariants live here, as an explicit checklist --
// one CHECK per rule that must survive a rewrite. Adding a rule to the seed
// without adding it here is not a test failure (that would be noise), but losing
// one IS.
#include "doctest.h"

#include "core/prompts.h"
#include "core/tool_list_parser.h"

#include <string>
#include <vector>

namespace {

bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// Position of `a` before `b`, used for "the answer follows this order".
bool ordered(const std::string& hay, const std::string& a, const std::string& b) {
    const size_t pa = hay.find(a);
    const size_t pb = hay.find(b);
    return pa != std::string::npos && pb != std::string::npos && pa < pb;
}

} // namespace

TEST_CASE("role seed: the default file name and the startup index") {
    CHECK(std::string(defaultPromptFile()) == "assistant.md");

    CHECK(defaultPromptIndex({}) == -1);
    CHECK(defaultPromptIndex({"assistant.md"}) == 0);
    CHECK(defaultPromptIndex({"reviewer.md", "assistant.md"}) == 1);
    // The seeded file is gone but the list is not: first entry wins.
    CHECK(defaultPromptIndex({"reviewer.md"}) == 0);
    // No role name is reserved: a file called coder.md is a normal role, it just
    // is not preferred over the seed.
    CHECK(defaultPromptIndex({"analyzer.md", "assistant.md"}) == 1);
    CHECK(defaultPromptIndex({"coder.md"}) == 0);
}

TEST_CASE("role seed: no tool inventory and no allowlist trigger") {
    const std::string seed = defaultPromptSeedText();

    // The inventory moved to the per-turn capability map. A second copy in the
    // prompt is a copy that drifts -- it already had (diagram_tool was listed even
    // when no pytool was installed).
    CHECK_FALSE(has(seed, "## 5. Available Tools"));
    CHECK_FALSE(has(seed, "## Available Tools"));

    // The allowlist is parsed by searching for this exact string. If the seed
    // explains the mechanism and accidentally contains it, parseAvailableTools()
    // reads the surrounding prose as a tool list and disables every real tool.
    CHECK_FALSE(has(seed, "tools available:"));
    CHECK(parseAvailableTools(seed).empty());

    // The old seed claimed a 500-character tool-result cap. The real cap is
    // 32000; the wrong number made the model detour through exec_shell.
    CHECK_FALSE(has(seed, "500 chars"));
}

TEST_CASE("role seed: the four-part answer shape, in order") {
    const std::string seed = defaultPromptSeedText();
    CHECK(has(seed, "## Output shape (highest priority)"));
    CHECK(has(seed, "1. Conclusion"));
    CHECK(has(seed, "2. What it means for the user"));
    CHECK(has(seed, "3. Details"));
    CHECK(has(seed, "4. Next step"));
    CHECK(ordered(seed, "1. Conclusion", "2. What it means for the user"));
    CHECK(ordered(seed, "2. What it means for the user", "3. Details"));
    CHECK(ordered(seed, "3. Details", "4. Next step"));
    // The whole point: plain language outranks the professional detail dump.
    CHECK(has(seed, "new graduate can follow"));
    CHECK(ordered(seed, "## Output shape", "## Working style"));
}

TEST_CASE("role seed: the hard rules that must survive any rewrite") {
    const std::string seed = defaultPromptSeedText();
    const std::vector<const char*> must = {
        // formatting
        "Pure Markdown only",
        "NO HTML, NO Emoji",
        "NEVER put blank lines inside tables",
        // language
        "Match the language of the session and the project",
        "mirror the user's most recent message",
        "ONLY English and ASCII",
        // precision
        "exact file paths, function signatures",
        // working style
        "Never patch item by item",
        "update_todo(action=\"init\")",
        "Do not work outside the plan",
        "Read before writing",
        "Batch independent tool calls",
        "thread safety (mutex, atomic)",
        "lifetimes (new/delete, RAII, ownership)",
        "render paths, callback chains",
        "build it, run the tests",
        "Output that looks cut off is cut off",
        "Silence is not failure",
        // what may be changed / when it activates
        "projv_files is writable",
        "sessions/ and logs/ are read only",
        "next F3",
        "your next message",
        // documents
        "md_file only",
        "1. Background & Goals",
        "4. Changes List (File | Action | Description | ~Lines)",
        "5. Risks",
        "up to 8KB",
        // diagrams
        "rankdir=TB",
        "rankdir=LR",
        "label the edges with types and structs",
        "ownership plus mutex/atomic annotations",
        "Skip diagrams for a trivial one-file change",
    };
    for (const char* rule : must) CHECK_MESSAGE(has(seed, rule), rule);
}

TEST_CASE("role seed: ASCII only, and a sane size") {
    const std::string seed = defaultPromptSeedText();
    for (unsigned char c : seed) CHECK_MESSAGE(c < 128, static_cast<int>(c));
    CHECK(seed.size() > 2000);
    CHECK(seed.size() < 8 * 1024); // it goes into every request of every turn
    CHECK(has(seed, "\n"));        // a single-line prompt is a broken raw string
}
