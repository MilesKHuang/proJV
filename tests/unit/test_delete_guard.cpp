// delete_guard: the pure classifier behind the shell approval dialog.
//
// Two directions matter equally here. A miss (dangerous command classified as
// Allow) destroys data; a false alarm (read-only command classified as Ask)
// trains the user to click through dialogs without reading them.
#include "doctest.h"

#include "core/delete_guard.h"

#include <string>
#include <vector>

using delete_guard::Decision;

namespace {
Decision classify(const std::string& cmd) {
    return delete_guard::evaluate(cmd).decision;
}
}

TEST_CASE("delete_guard: splitSegments keeps quoted operators together") {
    const auto v = delete_guard::splitSegments("git commit -m \"fix & feature\" && ls");
    REQUIRE(v.size() == 2);
    CHECK(v[0] == "git commit -m \"fix & feature\"");
    CHECK(v[1] == "ls");
}

TEST_CASE("delete_guard: splitSegments covers ; | || and newlines") {
    CHECK(delete_guard::splitSegments("a; b").size() == 2);
    CHECK(delete_guard::splitSegments("a | b").size() == 2);
    CHECK(delete_guard::splitSegments("a || b").size() == 2);
    CHECK(delete_guard::splitSegments("a\nb").size() == 2);
}

TEST_CASE("delete_guard: splitSegments keeps Windows paths intact") {
    const auto v = delete_guard::splitSegments("rm C:\\tmp\\x");
    REQUIRE(v.size() == 1);
    CHECK(v[0] == "rm C:\\tmp\\x");
}

TEST_CASE("delete_guard: firstCommandToken peels wrappers") {
    CHECK(delete_guard::firstCommandToken("cmd /c del x") == "del");
    CHECK(delete_guard::firstCommandToken("cmd /s /c del x") == "del");
    CHECK(delete_guard::firstCommandToken("sudo rm -rf /") == "rm");
    CHECK(delete_guard::firstCommandToken("sh -c \"rm -rf x\"") == "rm");
    CHECK(delete_guard::firstCommandToken("/bin/rm x") == "rm");
    CHECK(delete_guard::firstCommandToken("FOO=1 rm x") == "rm");
    CHECK(delete_guard::firstCommandToken("Format.com D:") == "format");
}

TEST_CASE("delete_guard: red lines are Deny") {
    CHECK(classify("format D:") == Decision::Deny);
    CHECK(classify("diskpart") == Decision::Deny);
    CHECK(classify("Format-Volume -DriveLetter D") == Decision::Deny);
    CHECK(classify("mkfs.ext4 /dev/sdb1") == Decision::Deny);
    CHECK(classify("fdisk /dev/sda") == Decision::Deny);
    CHECK(classify("dd if=/dev/zero of=/dev/sda bs=1M") == Decision::Deny);
    CHECK(classify("diskutil eraseDisk APFS X /dev/disk2") == Decision::Deny);
    CHECK(classify("sudo mkfs -t ext4 /dev/sdb1") == Decision::Deny);
    CHECK(classify("cls && format D:") == Decision::Deny);
}

TEST_CASE("delete_guard: formatting tools are not red lines") {
    // The whole point of matching the command token instead of a substring:
    // these three are everyday commands in this repository.
    CHECK(classify("clang-format -i src/a.cpp") == Decision::Allow);
    CHECK(classify("git format-patch -1") == Decision::Allow);
    CHECK(classify("npm run format") == Decision::Allow);
    CHECK(classify("format_file") == Decision::Allow);
}

TEST_CASE("delete_guard: deletes are Ask") {
    CHECK(classify("rm -rf build") == Decision::Ask);
    CHECK(classify("del x.txt") == Decision::Ask);
    CHECK(classify("rmdir /s /q out") == Decision::Ask);
    CHECK(classify("Remove-Item -Recurse x") == Decision::Ask);
    CHECK(classify("rd /s /q bin") == Decision::Ask);
}

TEST_CASE("delete_guard: wrapped deletes are still Ask") {
    CHECK(classify("cmd /c del x") == Decision::Ask);
    CHECK(classify("powershell -c \"Remove-Item x\"") == Decision::Ask);
    CHECK(classify("sudo rm -rf ~") == Decision::Ask);
    CHECK(classify("echo x | rm -rf ~") == Decision::Ask);
    CHECK(classify("python -c \"os.remove('a')\"") == Decision::Ask);
    CHECK(classify("node -e \"require('fs').unlinkSync('a')\"") == Decision::Ask);
}

TEST_CASE("delete_guard: read-only inline interpreter code is Allow") {
    // The rule used to be "any -c/-e asks", which put a dialog in front of every
    // diagnostic one-liner (reading a SQLite file, formatting JSON). Only the
    // primitives that can destroy data ask now.
    CHECK(classify("py -3 -c \"import sqlite3; print(sqlite3.sqlite_version)\"") ==
          Decision::Allow);
    CHECK(classify("python -c \"import json; print(json.dumps({}))\"") == Decision::Allow);
    CHECK(classify("python -c \"print('{}'.format(1))\"") == Decision::Allow);
    CHECK(classify("python -c \"items=[]; items.remove(1)\"") == Decision::Allow);
    // A write-mode open() and the usual destructive APIs still ask.
    CHECK(classify("python -c \"open('x','w').write('y')\"") == Decision::Ask);
    CHECK(classify("python -c \"import shutil; shutil.rmtree('build')\"") == Decision::Ask);
    CHECK(classify("python -c \"import os; os.system('del x')\"") == Decision::Ask);
    // A read-only payload with a redirect is still an overwrite of the target.
    CHECK(classify("python -c \"print(1)\" > out.txt") == Decision::Ask);
}

TEST_CASE("delete_guard: git destruction is Ask") {
    CHECK(classify("git clean -fdx") == Decision::Ask);
    CHECK(classify("git reset --hard HEAD~3") == Decision::Ask);
    CHECK(classify("git checkout -- src") == Decision::Ask);
    CHECK(classify("git stash drop") == Decision::Ask);
    // Non-destructive git stays Allow.
    CHECK(classify("git status") == Decision::Allow);
    CHECK(classify("git checkout -b feature") == Decision::Allow);
    CHECK(classify("git diff HEAD~1") == Decision::Allow);
    CHECK(classify("git log --oneline -5") == Decision::Allow);
}

TEST_CASE("delete_guard: history-rewriting git commands ask first") {
    // commit and push do not delete anything, but they are irreversible in
    // practice: the user must read the exact command before it runs.
    CHECK(classify("git commit -m \"fix\"") == Decision::Ask);
    CHECK(classify("git commit -am \"wip\"") == Decision::Ask);
    CHECK(classify("git push") == Decision::Ask);
    CHECK(classify("git push origin main") == Decision::Ask);
    CHECK(classify("git push --force-with-lease") == Decision::Ask);
    // Inside a pipeline the whole segment is judged, so the gate still fires.
    CHECK(classify("git add -A && git commit -m \"x\"") == Decision::Ask);
}

TEST_CASE("delete_guard: overwrite asks only when the target can exist") {
    const auto v = delete_guard::evaluate("cp a.txt b.txt");
    CHECK(v.decision == Decision::Ask);
    CHECK(v.needsExistenceCheck);
    CHECK(v.checkTarget == "b.txt");

    const auto plain = delete_guard::evaluate("cp");
    CHECK(plain.decision == Decision::Ask);
    CHECK_FALSE(plain.needsExistenceCheck);
}

TEST_CASE("delete_guard: redirects to a real file are Ask") {
    CHECK(classify("echo hi > notes.txt") == Decision::Ask);
    CHECK(classify("echo hi >> notes.txt") == Decision::Ask);
    CHECK(classify("build.bat > log.txt") == Decision::Ask);
}

TEST_CASE("delete_guard: harmless redirects are Allow") {
    CHECK(classify("cmake --build build > /dev/null") == Decision::Allow);
    CHECK(classify("cmd /c dir > nul") == Decision::Allow);
    CHECK(classify("run.exe 2>&1") == Decision::Allow);
}

TEST_CASE("delete_guard: read-only and build commands are Allow") {
    CHECK(classify("ls -la") == Decision::Allow);
    CHECK(classify("git status") == Decision::Allow);
    CHECK(classify("grep -r x .") == Decision::Allow);
    CHECK(classify("cmake --build build") == Decision::Allow);
    CHECK(classify("npm install") == Decision::Allow);
    CHECK(classify("python build.py") == Decision::Allow);
    CHECK(classify("") == Decision::Allow);
}

TEST_CASE("delete_guard: shutdown and taskkill are deliberately Allow") {
    // A process or a machine restarts; a partition table does not. These were
    // in the old keyword table and are intentionally not asked any more.
    CHECK(classify("shutdown /s /t 0") == Decision::Allow);
    CHECK(classify("taskkill /F /IM node.exe") == Decision::Allow);
}

TEST_CASE("delete_guard: mixed batches take the worst verdict") {
    CHECK(classify("ls && rm -rf x") == Decision::Ask);
    CHECK(classify("rm -rf x && format D:") == Decision::Deny);
    CHECK(classify("echo ok") == Decision::Allow);
}

TEST_CASE("delete_guard: deeply nested wrappers fall back to Ask") {
    std::string cmd;
    for (int i = 0; i < 10; ++i)
        cmd += "sudo ";
    cmd += "rm -rf /";
    CHECK(classify(cmd) == Decision::Ask);
}
