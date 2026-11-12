// cli_args: the two meta commands that must run before anything else in main().
#include "doctest.h"

#include "platform/cli_args.h"

#include <string>
#include <vector>

TEST_CASE("cli_args: --version exits 0 and prints the version") {
    std::string out;
    const auto rc = cli_args::parseMetaArgs({"--version"}, out);
    REQUIRE(rc.has_value());
    CHECK(*rc == 0);
    CHECK(out.find("proJV " + std::string(PROJV_VERSION)) != std::string::npos);
    // --version stays short: no usage block.
    CHECK(out.find("--install") == std::string::npos);
}

TEST_CASE("cli_args: -V is the short form of --version") {
    std::string out;
    const auto rc = cli_args::parseMetaArgs({"-V"}, out);
    REQUIRE(rc.has_value());
    CHECK(*rc == 0);
    CHECK(out.find("proJV") != std::string::npos);
}

TEST_CASE("cli_args: --help lists both command groups") {
    std::string out;
    const auto rc = cli_args::parseMetaArgs({"--help"}, out);
    REQUIRE(rc.has_value());
    CHECK(*rc == 0);
    CHECK(out.find("Usage:") != std::string::npos);
    CHECK(out.find("--install") != std::string::npos);
    CHECK(out.find("--mcp-check") != std::string::npos);
    CHECK(out.find(PROJV_VERSION) != std::string::npos);
}

TEST_CASE("cli_args: -h is the short form of --help") {
    std::string out;
    const auto rc = cli_args::parseMetaArgs({"-h"}, out);
    REQUIRE(rc.has_value());
    CHECK(out.find("Usage:") != std::string::npos);
}

TEST_CASE("cli_args: --help wins over --install") {
    // `projv --install --help` must print help, not start the installer.
    std::string out;
    const auto rc = cli_args::parseMetaArgs({"--install", "--help"}, out);
    REQUIRE(rc.has_value());
    CHECK(out.find("Usage:") != std::string::npos);
}

TEST_CASE("cli_args: everything else falls through to main()") {
    std::string out;
    CHECK_FALSE(cli_args::parseMetaArgs({}, out).has_value());
    CHECK_FALSE(cli_args::parseMetaArgs({"--nonsense"}, out).has_value());
    CHECK_FALSE(cli_args::parseMetaArgs({"--mcp-check"}, out).has_value());
    CHECK_FALSE(cli_args::parseMetaArgs({"--install", "pkg.zip"}, out).has_value());
    // Nothing consumed -> nothing printed.
    CHECK(out.empty());
}
