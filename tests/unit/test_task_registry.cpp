// proJV -- background tasks (src/core/task_registry.h).
//
// What is asserted here is what the dock and the model depend on:
//   * the dock's one-liner is the LATEST non-empty line, not the first;
//   * a task's output is readable after it exits, and shaped tail-first;
//   * ids are stable, unknown ids fail loudly, and a finished task cannot be killed.
// A real child process is spawned (echo) because the whole point is the file-backed
// output path; the fake_mcp_server fixture proves the harness can do that.
#include "doctest/doctest.h"
#include "core/task_registry.h"

#include <chrono>
#include <string>
#include <thread>

namespace {

bool waitFinished(TaskRegistry& reg, int id, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        for (int done : reg.takeFinished())
            if (done == id) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

} // namespace

TEST_CASE("bg_task: latestLine takes the last non-empty line") {
    CHECK(bg_task::latestLine("one\ntwo\nthree\n", 80) == "three");
    CHECK(bg_task::latestLine("one\ntwo\n\n\n", 80) == "two"); // trailing blanks ignored
    CHECK(bg_task::latestLine("single", 80) == "single");
    CHECK(bg_task::latestLine("", 80).empty());
    CHECK(bg_task::latestLine("\r\n\r\n", 80).empty());
    // CRLF and leading indentation are trimmed.
    CHECK(bg_task::latestLine("a\r\n  b\r\n", 80) == "b");
}

TEST_CASE("bg_task: latestLine truncates without cutting a UTF-8 character") {
    const std::string line = "added 42 packages in 3s";
    CHECK(bg_task::latestLine(line, 10) == "added 42 p...");
    // 3-byte characters: the cut must land on a boundary.
    const std::string cjk = "\xe4\xb8\xad\xe6\x96\x87\xe6\xb5\x8b\xe8\xaf\x95";
    const std::string cut = bg_task::latestLine(cjk, 4);
    REQUIRE(cut.size() > 3);
    CHECK((static_cast<unsigned char>(cut[0]) & 0xC0) != 0x80);
    // 4 bytes holds one whole 3-byte character plus "...".
    CHECK(cut == "\xe4\xb8\xad...");
}

TEST_CASE("bg_task: state labels are stable strings") {
    CHECK(std::string(bg_task::stateLabel(bg_task::State::Running)) == "running");
    CHECK(std::string(bg_task::stateLabel(bg_task::State::Done)) == "done");
    CHECK(std::string(bg_task::stateLabel(bg_task::State::Failed)) == "failed");
    CHECK(std::string(bg_task::stateLabel(bg_task::State::Killed)) == "killed");
    CHECK(std::string(bg_task::stateLabel(bg_task::State::TimedOut)) == "timed out");
}

TEST_CASE("bg_task: a real child's output is readable after it exits") {
    TaskRegistry reg;
    std::string err;
    const int id = reg.start("echo projv-background-probe", "", err);
    REQUIRE_MESSAGE(id > 0, err);
    REQUIRE(waitFinished(reg, id, 15000));

    std::string readErr;
    const std::string out = reg.read(id, 400, false, readErr);
    CHECK(readErr.empty());
    CHECK(out.find("projv-background-probe") != std::string::npos);

    // The snapshot reports the terminal state and the command for the dock.
    bool found = false;
    for (const auto& s : reg.snapshots()) {
        if (s.id != id) continue;
        found = true;
        CHECK(s.state == bg_task::State::Done);
        CHECK(s.exitCode == 0);
        CHECK(s.command.find("projv-background-probe") != std::string::npos);
        CHECK(s.outputBytes > 0);
    }
    CHECK(found);
    CHECK(reg.runningCount() == 0);
}

TEST_CASE("bg_task: ids increment and unknown ids fail loudly") {
    TaskRegistry reg;
    std::string err;
    const int a = reg.start("echo a", "", err);
    REQUIRE_MESSAGE(a > 0, err);
    std::string err2;
    const int b = reg.start("echo b", "", err2);
    REQUIRE_MESSAGE(b > a, "a=" << a << " b=" << b << " err=" << err2);

    std::string readErr;
    CHECK(reg.read(9999, 400, false, readErr).empty());
    CHECK(readErr.find("no task with id 9999") != std::string::npos);

    std::string endErr;
    CHECK_FALSE(reg.end(9999, endErr));
    CHECK(endErr.find("no task with id 9999") != std::string::npos);
}

TEST_CASE("bg_task: a finished task cannot be killed twice") {
    TaskRegistry reg;
    std::string err;
    const int id = reg.start("echo done", "", err);
    REQUIRE(id > 0);
    REQUIRE(waitFinished(reg, id, 15000));

    std::string endErr;
    CHECK_FALSE(reg.end(id, endErr));
    CHECK(endErr.find("already finished") != std::string::npos);
}

TEST_CASE("bg_task: shutdown drops every task") {
    TaskRegistry reg;
    std::string err;
    REQUIRE(reg.start("echo x", "", err) > 0);
    CHECK_FALSE(reg.empty());
    reg.shutdown();
    CHECK(reg.empty());
    CHECK(reg.snapshots().empty());
}
