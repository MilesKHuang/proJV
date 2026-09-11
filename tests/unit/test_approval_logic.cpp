// Approval dialog logic tests.
#include "doctest.h"

#include "tui/approval_logic.h"

#include <string>
#include <vector>

TEST_CASE("approval_logic: extractCommand from JSON") {
    CHECK(approval_logic::extractCommand(R"({"command":"rm file.txt"})") == "rm file.txt");
}

TEST_CASE("approval_logic: extractCommand falls back to raw args") {
    std::string raw = "not json";
    CHECK(approval_logic::extractCommand(raw) == raw);
}

TEST_CASE("approval_logic: extractDeleteFiles basic") {
    auto files = approval_logic::extractDeleteFiles("rm file1.txt file2.txt");
    REQUIRE(files.size() == 2);
    CHECK(files[0] == "file1.txt");
    CHECK(files[1] == "file2.txt");
}

TEST_CASE("approval_logic: extractDeleteFiles quoted path") {
    auto files = approval_logic::extractDeleteFiles("del \"my file.txt\" /f");
    REQUIRE(files.size() == 1);
    CHECK(files[0] == "my file.txt");
}

TEST_CASE("approval_logic: extractDeleteFiles skips options") {
    auto files = approval_logic::extractDeleteFiles("rm -rf build");
    REQUIRE(files.size() == 1);
    CHECK(files[0] == "build");
}

TEST_CASE("approval_logic: extractDeleteFiles no keyword") {
    auto files = approval_logic::extractDeleteFiles("echo hello");
    CHECK(files.empty());
}
