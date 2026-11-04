// proJV unit tests -- install / shim management.
//
// The rules that decide what an install may touch are pure functions, so they
// are tested here without writing to a real user profile. The filesystem tests
// use PROJV_HOME / PROJV_SHIM_DIR style overrides pointed at a temp directory,
// which is exactly what the design requires for testability.
#include "doctest/doctest.h"

#include "platform/install_shim.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using namespace install_shim;

namespace {

struct TempDir {
    fs::path path;

    TempDir() {
        static int counter = 0;
        path = fs::temp_directory_path() / ("projv-install-test-" + std::to_string(++counter) +
                                            "-" + std::to_string(std::rand()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    std::string str() const { return path.string(); }
};

void writeFileAt(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << text;
}

std::string readFileAt(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// The shim name is platform data, not a test constant: `projv.cmd` on Windows,
// an extension-less script everywhere else (install_shim::writeShim). Asking the
// same question the implementation asks is what keeps this file green on both.
std::string shimFile(const std::string& base) {
#ifdef _WIN32
    return base + ".cmd";
#else
    return base;
#endif
}

// The installed binary follows the same rule: `.exe` on Windows, bare elsewhere
// (CMake installs `proJV_tui${CMAKE_EXECUTABLE_SUFFIX}`).
std::string exeName() {
#ifdef _WIN32
    return "proJV_tui.exe";
#else
    return "proJV_tui";
#endif
}

} // namespace

TEST_CASE("install: copy rules protect user data and upgrade the binary") {
    CHECK(copy_rule("projv_files/config.toml") == CopyRule::Never);
    CHECK(copy_rule("projv_files/sessions/chat.db") == CopyRule::Never);
    CHECK(copy_rule("projv_files/logs/proJV.log") == CopyRule::Never);
    CHECK(copy_rule("projv_files/prompts/coder.md") == CopyRule::Never);

    CHECK(copy_rule("proJV_tui.exe") == CopyRule::Overwrite);
    CHECK(copy_rule("README.md") == CopyRule::Overwrite);
    CHECK(copy_rule("README_zh.md") == CopyRule::Overwrite);
    CHECK(copy_rule("LICENSE") == CopyRule::Overwrite);

    CHECK(copy_rule("projv_files/theme/forest.json") == CopyRule::KeepIfMissing);
    CHECK(copy_rule("projv_files/skills/monica/config.json") == CopyRule::KeepIfMissing);
    CHECK(copy_rule("projv_files/pytool/__init__.py") == CopyRule::KeepIfMissing);

    // Windows separators and case must not change the verdict.
    CHECK(copy_rule("projv_files\\Config.TOML") == CopyRule::Never);
    CHECK(copy_rule("PROJV_FILES/sessions/x.pjs") == CopyRule::Never);

    // Only the runtime payload ships: build artefacts never reach a user.
    CHECK(copy_rule("proJV_lib.lib") == CopyRule::Never);
    CHECK(copy_rule("proJV_tui.pdb") == CopyRule::Never);
    CHECK(copy_rule("CMakeCache.txt") == CopyRule::Never);
    CHECK(copy_rule("CMakeLists.txt") == CopyRule::Never);
}
TEST_CASE("install: the shim forwards args and the exit code") {
    const std::string cmd = shim_cmd("projv", "C:\\Users\\a b\\Programs\\projv\\proJV_tui.exe");
    CHECK(cmd.find("@echo off") == 0);
    CHECK(cmd.find(kMarker) != std::string::npos);
    // The absolute path is quoted: a space in the profile name must not break it.
    CHECK(cmd.find("\"C:\\Users\\a b\\Programs\\projv\\proJV_tui.exe\" %*") != std::string::npos);
    CHECK(cmd.find("exit /b %ERRORLEVEL%") != std::string::npos);

    const std::string sh = shim_sh("/home/a/.local/share/projv/proJV_tui");
    CHECK(sh.find("#!/bin/sh") == 0);
    CHECK(sh.find(kMarker) != std::string::npos);
    // exec keeps signals and the exit code attached to the TUI.
    CHECK(sh.find("exec \"/home/a/.local/share/projv/proJV_tui\" \"$@\"") != std::string::npos);
}

TEST_CASE("install: the marker decides what --uninstall may delete") {
    CHECK(has_marker(shim_cmd("projv", "x")));
    CHECK(has_marker(shim_sh("x")));
    CHECK_FALSE(has_marker("@echo off\n\"C:\\other\\thing.exe\" %*\n"));
    CHECK_FALSE(has_marker(""));
}

TEST_CASE("install: PATH merge happens once and keeps the existing order") {
#ifdef _WIN32
    const std::string dir = "C:\\Users\\a\\AppData\\Local\\Microsoft\\WindowsApps";
    const std::string other = "C:\\Windows;C:\\Windows\\System32";
    const std::string trailing = dir + "\\";
    const std::string downcased = "c:\\users\\a\\appdata\\local\\microsoft\\windowsapps";
#else
    // The separator is ':' here, and a Windows-style path carries a colon of its
    // own, so the sample data has to be native: a Windows list would be split in
    // the wrong places and prove nothing about this host.
    const std::string dir = "/home/a/.local/bin";
    const std::string other = "/usr/local/bin:/usr/bin";
    const std::string trailing = dir + "/";
    const std::string downcased = "/HOME/a/.local/bin";
#endif

    CHECK_FALSE(path_contains(other, dir));
    const std::string merged = merge_path(other, dir);
    CHECK(path_contains(merged, dir));
    CHECK(merged.compare(0, other.size(), other) == 0); // no reordering
    CHECK(merge_path(merged, dir) == merged);           // idempotent
    CHECK(path_contains(merged, trailing));             // trailing separator
    CHECK(path_contains(merged, downcased));            // case is compared folded
    CHECK(merge_path("", dir) == dir);
}

TEST_CASE("install: is_inside guards --purge") {
    const std::string home = (fs::temp_directory_path() / "projv-purge-guard").string();
    CHECK(is_inside(home, home));
    CHECK(is_inside((fs::path(home) / "projv_files").string(), home));
    CHECK_FALSE(is_inside(fs::temp_directory_path().string(), home));
}

TEST_CASE("install: the summary block states the four facts the doc promises") {
    const std::string text =
        summary("D:\\Apps\\projv", "C:\\WindowsApps\\projv.cmd", true, "C:\\WindowsApps\\projv.cmd",
                "cd <project> && projv", "projv_tui --uninstall");
    CHECK(text.find("installed : D:\\Apps\\projv") != std::string::npos);
    CHECK(text.find("(in PATH: yes)") != std::string::npos);
    CHECK(text.find("next      : cd <project> && projv") != std::string::npos);
    CHECK(text.find("undo      : projv_tui --uninstall") != std::string::npos);

    const std::string missing = summary("h", "", false, "", "n", "u");
    CHECK(missing.find("(in PATH: no)") != std::string::npos);
    CHECK(missing.find("(not on PATH yet)") != std::string::npos);
}

TEST_CASE("install: a second install upgrades the binary and keeps user files") {
    TempDir package;
    TempDir target;
    TempDir shims;

    // A package as a downloaded ZIP would look: binary, docs, theme, and a
    // config the developer happened to have locally.
    writeFileAt(package.path / exeName(), "v1");
    writeFileAt(package.path / "README.md", "readme v1");
    writeFileAt(package.path / "projv_files" / "theme" / "forest.json", "theme v1");
    writeFileAt(package.path / "projv_files" / "config.toml", "api_key = SECRET");

    Options opt;
    opt.home = target.str();
    opt.shimDir = shims.str();
    opt.exePath = (package.path / exeName()).string();
    opt.exeDir = package.str();

    REQUIRE(run_install(opt) == 0);
    CHECK(readFileAt(target.path / exeName()) == "v1");
    CHECK(readFileAt(target.path / "projv_files" / "theme" / "forest.json") == "theme v1");
    // The developer's key is never copied into the install.
    CHECK_FALSE(fs::exists(target.path / "projv_files" / "config.toml"));
    // Both shim names exist.
    CHECK(fs::exists(shims.path / shimFile("projv")));
    CHECK(fs::exists(shims.path / shimFile("projv_tui")));

    // The user edits a theme and gains a session; a second install keeps both.
    writeFileAt(target.path / "projv_files" / "theme" / "forest.json", "user edit");
    writeFileAt(target.path / "projv_files" / "sessions" / "chat.db", "history");
    writeFileAt(package.path / exeName(), "v2");

    REQUIRE(run_install(opt) == 0);
    CHECK(readFileAt(target.path / exeName()) == "v2"); // upgraded
    CHECK(readFileAt(target.path / "projv_files" / "theme" / "forest.json") == "user edit");
    CHECK(readFileAt(target.path / "projv_files" / "sessions" / "chat.db") == "history");

    // --uninstall removes only our shims and keeps the data.
    REQUIRE(run_uninstall(opt) == 0);
    CHECK_FALSE(fs::exists(shims.path / shimFile("projv")));
    CHECK(fs::exists(target.path / "projv_files" / "sessions" / "chat.db"));
}

TEST_CASE("install: --uninstall refuses a foreign file with the same name") {
    TempDir target;
    TempDir shims;
    // Somebody else's file with our name: no marker, so uninstall must not touch it.
    writeFileAt(shims.path / shimFile("projv"), "@echo off\r\n\"C:\\other\\thing.exe\" %*\r\n");

    Options opt;
    opt.home = target.str();
    opt.shimDir = shims.str();

    CHECK(run_uninstall(opt) == 1); // refused, non-zero
    CHECK(fs::exists(shims.path / shimFile("projv")));
}

TEST_CASE("install: --post-install only writes the shims") {
    TempDir target;
    TempDir shims;
    writeFileAt(target.path / exeName(), "installed by cmake");

    Options opt;
    opt.home = target.str();
    opt.shimDir = shims.str();
    REQUIRE(run_post_install(opt) == 0);

    // The shim has to point at the binary the install just dropped. On POSIX
    // writeShim prefers a symlink, so the link target is the answer there; where
    // a symlink cannot be created it is a script that mentions the path.
    const fs::path shim = shims.path / shimFile("projv");
    REQUIRE(fs::exists(shim));
    const std::string want = (target.path / exeName()).string();
    if (fs::is_symlink(shim))
        CHECK(fs::read_symlink(shim).string() == want);
    else
        CHECK(readFileAt(shim).find(want) != std::string::npos);
}

TEST_CASE("install: --purge removes the tree but refuses from inside it") {
    TempDir target;
    TempDir shims;
    writeFileAt(target.path / "projv_files" / "theme" / "a.json", "x");

    Options opt;
    opt.home = target.str();
    opt.shimDir = shims.str();
    opt.exePath = (fs::temp_directory_path() / "elsewhere" / exeName()).string();

    const fs::path saved = fs::current_path();
    fs::current_path(target.path); // stand inside the install directory
    const int inside = run_purge(opt);
    fs::current_path(saved);
    CHECK(inside == 2);
    CHECK(fs::exists(target.path)); // nothing deleted

    CHECK(run_purge(opt) == 0);
    CHECK_FALSE(fs::exists(target.path / "projv_files"));
}
