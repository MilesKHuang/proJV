// proJV -- the approval policy: which commands still reach the dialog.
//
// approval_mode is the dial: strict asks for everything, balanced (the default) lets
// functional overwrites through, loose only refuses the red line. The per-command
// split must hold in every mode, so the same commands are checked mode by mode.
#include "doctest/doctest.h"
#include "client/deepseek.h"
#include "core/agent.h"
#include "tools/registry.h"

#include <filesystem>
#include <fstream>
#include <string>

TEST_CASE("approval: balanced (the default) lets functional overwrites run") {
    DeepSeekClient client;
    ToolRegistry tools;
    Agent agent(client, tools);
    CHECK(agent.approvalMode() == ApprovalMode::Balanced); // default

    // This is the case that produced a dozen dialogs per build session.
    CHECK_FALSE(agent.policyNeedsApproval("cmake --build build --parallel > build\\_a.log 2>&1"));
    CHECK_FALSE(agent.policyNeedsApproval("ctest --test-dir build -C Release > out.txt"));
    CHECK_FALSE(agent.policyNeedsApproval("echo hello > log.txt"));
    CHECK_FALSE(agent.policyNeedsApproval("dir >> build\\listing.txt"));
    // Nothing dangerous at all.
    CHECK_FALSE(agent.policyNeedsApproval("cmake --build build --parallel"));
    CHECK_FALSE(agent.policyNeedsApproval("git status"));
    CHECK_FALSE(agent.policyNeedsApproval(""));
}

TEST_CASE("approval: destructive commands ask even in balanced mode") {
    DeepSeekClient client;
    ToolRegistry tools;
    Agent agent(client, tools);

    CHECK(agent.policyNeedsApproval("rm -rf build"));
    CHECK(agent.policyNeedsApproval("del important.txt"));
    CHECK(agent.policyNeedsApproval("git clean -fdx"));
    CHECK(agent.policyNeedsApproval("git reset --hard HEAD~3"));
    CHECK(agent.policyNeedsApproval("git commit -m \"fix\""));
    CHECK(agent.policyNeedsApproval("git push"));
    CHECK(agent.policyNeedsApproval("python -c \"import os; os.remove('x')\""));
}

TEST_CASE("approval: strict asks for everything and offers no ALWAYS") {
    DeepSeekClient client;
    ToolRegistry tools;
    Agent agent(client, tools);
    agent.setApprovalMode(ApprovalMode::Strict);
    CHECK(agent.approvalMode() == ApprovalMode::Strict);
    CHECK_FALSE(agent.canAlways());

    CHECK(agent.policyNeedsApproval("echo hello > log.txt"));
    CHECK(agent.policyNeedsApproval("cmake --build build --parallel > build\\_a.log 2>&1"));
    CHECK(agent.policyNeedsApproval("rm -rf build"));
    // Still no dialog for a plain command: Allow never asked.
    CHECK_FALSE(agent.policyNeedsApproval("cmake --build build --parallel"));
}

TEST_CASE("approval: loose only refuses the red line") {
    DeepSeekClient client;
    ToolRegistry tools;
    Agent agent(client, tools);
    agent.setApprovalMode(ApprovalMode::Loose);
    CHECK(agent.canAlways());

    CHECK_FALSE(agent.policyNeedsApproval("rm -rf build"));
    CHECK_FALSE(agent.policyNeedsApproval("del important.txt"));
    CHECK_FALSE(agent.policyNeedsApproval("echo x > log.txt"));
    // Deny is outside the dial: format still never reaches a dialog.
    CHECK_FALSE(agent.policyNeedsApproval("mkfs.ext4 /dev/sdb1"));
}

TEST_CASE("approval: a remembered command never asks again") {
    DeepSeekClient client;
    ToolRegistry tools;
    Agent agent(client, tools);
    // Loaded from config.toml at startup.
    agent.setPersistentGrants({"rm -rf build"});
    CHECK(agent.policyNeedsApproval("rm -rf build") == false);
    CHECK(agent.policyNeedsApproval("rm -rf src") == true); // exact text only
    CHECK(agent.grants().size() == 1);

    agent.clearGrants();
    CHECK(agent.grants().empty());
    CHECK(agent.policyNeedsApproval("rm -rf build"));
}

TEST_CASE("approval: a red line is refused, never turned into a dialog") {
    DeepSeekClient client;
    ToolRegistry tools;
    Agent agent(client, tools);
    // policyNeedsApproval only covers Ask. Deny is handled before it, so the
    // answer here must be "no dialog" -- the command is refused outright.
    CHECK_FALSE(agent.policyNeedsApproval("mkfs.ext4 /dev/sdb1"));
    CHECK_FALSE(agent.policyNeedsApproval("dd if=/dev/zero of=/dev/sda"));
}

TEST_CASE("approval: cp is judged by whether the target already exists") {
    // The classifier is pure, so the existence check happens in the Agent. A copy
    // onto a new name destroys nothing; onto an existing file it does.
    DeepSeekClient client;
    ToolRegistry tools;
    Agent agent(client, tools);

    const std::string absent = "projv_policy_absent_target.txt";
    std::error_code ec;
    std::filesystem::remove(absent, ec);
    CHECK_FALSE(agent.policyNeedsApproval("cp a.txt " + absent));

    {
        std::ofstream out(absent, std::ios::binary | std::ios::trunc);
        out << "content that would be replaced";
    }
    CHECK(agent.policyNeedsApproval("cp a.txt " + absent));
    std::filesystem::remove(absent, ec);
}
