// Tests for the prompt-seed upgrades (prompts_loader.cpp).
// These transforms run on every startup against projv_files/prompts/*.md, so
// they must be idempotent and must never touch a hand-edited prompt.
#include "doctest.h"

#include "core/prompts.h"

#include <string>

namespace {

const char* kOldCoderLine =
    "tools available: read_file, write_file, exec_shell, grep_files, edit_file, "
    "file_search, web_search, fetch_url, md_file, update_todo, diagram_tool";

std::string coderWith(const std::string& toolsLine) {
    return std::string("# coder\n\n## 3. Available Tools\n") + toolsLine + "\n\n- tail\n";
}

} // namespace

TEST_CASE("prompt upgrade: old coder tool list gains run_skill exactly once") {
    const std::string oldText = coderWith(kOldCoderLine);
    const std::string once = upgradeSeededPrompt("coder.md", oldText);

    CHECK(once != oldText);
    CHECK(once.find(std::string(kOldCoderLine) + ", run_skill\n") != std::string::npos);

    // Idempotent: the upgraded line starts with the old one, so a naive
    // find() would append a second run_skill. It must not.
    const std::string twice = upgradeSeededPrompt("coder.md", once);
    CHECK(twice == once);
    CHECK(twice.find("run_skill, run_skill") == std::string::npos);

    // Count occurrences of the suffix.
    size_t pos = 0, hits = 0;
    while ((pos = twice.find("run_skill", pos)) != std::string::npos) {
        ++hits;
        pos += 9;
    }
    CHECK(hits == 1);
}

TEST_CASE("prompt upgrade: a hand-edited tool list is left alone") {
    const std::string custom = coderWith("tools available: read_file, md_file");
    CHECK(upgradeSeededPrompt("coder.md", custom) == custom);

    // Same list plus something the user added by hand.
    const std::string extended = coderWith(std::string(kOldCoderLine) + ", my_tool");
    CHECK(upgradeSeededPrompt("coder.md", extended) == extended);
}

TEST_CASE("prompt upgrade: compactor gains the image marker instruction once") {
    const std::string oldText =
        "# compactor\n\n- Do NOT fabricate information not present in the input.\n"
        "- Keep it short.\n";

    const std::string once = upgradeSeededPrompt("compactor.md", oldText);
    CHECK(once != oldText);
    CHECK(once.find("[+N images]") != std::string::npos);
    // Inserted after the anchor line, before "- Keep it short."
    CHECK(once.find("- Do NOT fabricate") < once.find("[+N images]"));
    CHECK(once.find("[+N images]") < once.find("- Keep it short."));

    const std::string twice = upgradeSeededPrompt("compactor.md", once);
    CHECK(twice == once);
}

TEST_CASE("prompt upgrade: a rewritten compactor prompt is left alone") {
    const std::string custom = "# my compactor\n\nDo whatever you want.\n";
    CHECK(upgradeSeededPrompt("compactor.md", custom) == custom);
}

TEST_CASE("prompt upgrade: unknown filenames pass through untouched") {
    const std::string text = coderWith(kOldCoderLine);
    CHECK(upgradeSeededPrompt("designer.md", text) == text);
    CHECK(upgradeSeededPrompt("anything.md", text) == text);
}
