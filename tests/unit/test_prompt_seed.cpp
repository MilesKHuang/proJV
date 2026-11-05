// Tests for the single universal role seed (assistant.md).
//
// The seed is the product: if it loses a hard rule, every session degrades and
// nothing else goes red. So the invariants live here -- the tool line, the
// two-layer expression rule, the plan/execute workflow, and the absence of the
// legacy role names this seed replaced.
#include "doctest.h"

#include "core/prompts.h"
#include "core/tool_list_parser.h"

#include <string>
#include <vector>

namespace {

bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
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

TEST_CASE("role seed: the tool line names every capability the role may use") {
    const std::string seed = defaultPromptSeedText();
    // coder's 13 + list_dir (planning) + format_file (end of an edit).
    const std::vector<std::string> want = {
        "read_file",    "write_file", "exec_shell", "grep_files", "edit_file",
        "file_search",  "web_search", "fetch_url",  "md_file",    "update_todo",
        "diagram_tool", "run_skill",  "view_image", "list_dir",   "format_file"};

    const std::vector<std::string> got = parseAvailableTools(seed);
    CHECK_MESSAGE(got.size() == want.size(), got.size());
    for (const auto& name : want) CHECK_MESSAGE(has(seed, name), name);

    // A prompt without the line means "all tools allowed" (agent.cpp), so an
    // accidental deletion must fail loudly here instead of silently widening.
    CHECK(has(seed, "tools available:"));
}

TEST_CASE("role seed: conclusion first, in the language the reader speaks") {
    const std::string seed = defaultPromptSeedText();
    CHECK(has(seed, "new graduate can follow"));
    CHECK(has(seed, "Match the language of the session and the project"));
    CHECK(has(seed, "Be concrete"));
    CHECK(has(seed, "ONLY English and ASCII"));

    // The two rules this seed replaced must not come back.
    CHECK(!has(seed, "Keep responses concise"));
    CHECK(!has(seed, "Chinese for narrative"));
}

TEST_CASE("role seed: the plan lives in the TODO list") {
    const std::string seed = defaultPromptSeedText();
    CHECK(has(seed, "update_todo(action=\"init\""));
    CHECK(has(seed, "update_todo(action=\"update\""));
    CHECK(has(seed, "update_todo(action=\"done\")"));
    CHECK(has(seed, "Confirm before acting when ambiguous"));
}

TEST_CASE("role seed: the unique rules of the three old roles survive") {
    const std::string seed = defaultPromptSeedText();
    CHECK(has(seed, "ONLY English and ASCII")); // coder
    CHECK(has(seed, "8KB"));                    // coder
    CHECK(has(seed, "timeout_ms"));             // coder
    CHECK(has(seed, "Be concrete"));            // designer
    CHECK(has(seed, "md_file ONLY"));           // designer
    CHECK(has(seed, "thread safety"));          // designer
    CHECK(has(seed, "cluster_"));               // analyzer
    CHECK(has(seed, "rankdir=TB"));             // analyzer
}

TEST_CASE("role seed: the legacy role names are gone") {
    const std::string seed = defaultPromptSeedText();
    for (const char* name : {"coder.md", "designer.md", "analyzer.md"})
        CHECK_MESSAGE(!has(seed, name), name);
}
