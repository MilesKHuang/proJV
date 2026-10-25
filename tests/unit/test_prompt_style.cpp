// Tests for the communication-style seed upgrade (prompts_loader.cpp).
// The style rule changed from a terse one-liner to a two-line rule (structure +
// plain language). Existing seeds must be upgraded once and never twice.
#include "doctest.h"

#include "core/prompts.h"

#include <string>

namespace {

const char* kOldYou = "- Answer like a human: conclusion first, then what it means for you; "
                      "then expose the core underlying logic.";
const char* kOldUser = "- Answer like a human: conclusion first, then what it means for the user; "
                       "then expose the core logic.";
const char* kOldBare = "- Answer like a human: conclusion first, then what it means; "
                       "then expose the core logic.";

const char* kNewFirst = "- Answer like a human: conclusion first, then what it means for the user, "
                        "then the core underlying logic.";
const char* kNewSecond = "- Put it in plain language";

std::string promptWith(const std::string& styleLine) {
    return "# role\n\n## Hard Rules\n- Pure Markdown only.\n" + styleLine +
           "\n\n## Workflow\n1. read\n";
}

size_t countSubstr(const std::string& hay, const std::string& needle) {
    size_t pos = 0, n = 0;
    while ((pos = hay.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

} // namespace

TEST_CASE("style upgrade: every seeded variant becomes the two-line rule") {
    const char* files[] = {"coder.md", "designer.md", "analyzer.md"};
    const char* olds[] = {kOldYou, kOldUser, kOldBare};

    for (const char* file : files) {
        for (const char* oldLine : olds) {
            const std::string before = promptWith(oldLine);
            const std::string after = upgradeSeededPrompt(file, before);

            CHECK_MESSAGE(after != before, file);
            CHECK_MESSAGE(after.find(kNewFirst) != std::string::npos, file);
            CHECK_MESSAGE(after.find(kNewSecond) != std::string::npos, file);
            // The old sentence is gone; the rest of the prompt is untouched.
            CHECK_MESSAGE(after.find(oldLine) == std::string::npos, file);
            CHECK_MESSAGE(after.find("- Pure Markdown only.") != std::string::npos, file);
            CHECK_MESSAGE(after.find("## Workflow") != std::string::npos, file);
            // The two lines stay adjacent, second one last.
            CHECK_MESSAGE(after.find(kNewFirst) < after.find(kNewSecond), file);
        }
    }
}

TEST_CASE("style upgrade: idempotent across repeated startups") {
    const std::string once = upgradeSeededPrompt("coder.md", promptWith(kOldYou));
    const std::string twice = upgradeSeededPrompt("coder.md", once);
    const std::string thrice = upgradeSeededPrompt("coder.md", twice);

    CHECK(twice == once);
    CHECK(thrice == once);
    CHECK(countSubstr(once, "Put it in plain language") == 1);
    CHECK(countSubstr(once, "core underlying logic") == 1);
}

TEST_CASE("style upgrade: a hand-written style rule is left alone") {
    const std::string custom =
        promptWith("- Answer like a human: lead with the answer, keep it short.");
    CHECK(upgradeSeededPrompt("coder.md", custom) == custom);
    CHECK(upgradeSeededPrompt("designer.md", custom) == custom);
    CHECK(upgradeSeededPrompt("analyzer.md", custom) == custom);
}

TEST_CASE("style upgrade: unknown prompt files are never touched") {
    const std::string text = promptWith(kOldYou);
    CHECK(upgradeSeededPrompt("my_role.md", text) == text);
    CHECK(upgradeSeededPrompt("compactor.md", text) == text); // different upgrade only
}

TEST_CASE("style upgrade: coder keeps its run_skill upgrade alongside the style one") {
    const std::string oldCoder =
        "# coder\n\n## Hard Rules\n" + std::string(kOldYou) +
        "\n"
        "tools available: read_file, write_file, exec_shell, grep_files, edit_file, "
        "file_search, web_search, fetch_url, md_file, update_todo, diagram_tool\n";
    const std::string upgraded = upgradeSeededPrompt("coder.md", oldCoder);

    CHECK(upgraded.find(kNewFirst) != std::string::npos);
    CHECK(upgraded.find(kNewSecond) != std::string::npos);
    CHECK(upgraded.find("diagram_tool, run_skill") != std::string::npos);
    CHECK(upgradeSeededPrompt("coder.md", upgraded) == upgraded);
}

TEST_CASE("style upgrade: an already-upgraded prompt is byte-identical") {
    // Exactly what the loader seeds today: both lines present.
    std::string text = "# role\n\n## Hard Rules\n" + std::string(kNewFirst) + "\n" + kNewSecond +
                       " -- make it as simple and easy to understand as possible, and "
                       "reduce the user's cognitive cost while retaining the necessary "
                       "professional details.\n\n## Workflow\n1. read\n";
    CHECK(upgradeSeededPrompt("coder.md", text) == text);
}
