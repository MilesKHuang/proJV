// log_files: the by-day log name and the 7-day retention rule.
//
// The rule that matters is the negative one: only files that match
// proJV-YYYY-MM-DD.log may ever be deleted. crash dumps, skill_metrics.log and
// the old proJV.log share the directory.
#include "doctest.h"

#include "platform/log_files.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path makeTempDir(const std::string& tag) {
    std::error_code ec;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path p = fs::temp_directory_path(ec) / ("projv_logtest_" + tag + "_" + std::to_string(stamp));
    fs::create_directories(p, ec);
    return p;
}

std::string dayString(int daysAgo) {
    const std::time_t now = std::time(nullptr) - static_cast<std::time_t>(daysAgo) * 86400;
    std::tm tm = {};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

void touch(const fs::path& p) {
    std::ofstream ofs(p, std::ios::app);
    ofs << "x\n";
}

} // namespace

TEST_CASE("log_files: todayLogPath carries today's date") {
    const std::string p = log_files::todayLogPath("C:/tmp/logs");
    CHECK(p.find("proJV-") != std::string::npos);
    CHECK(p.find(dayString(0)) != std::string::npos);
    CHECK(p.size() > 6);
    CHECK(p.compare(p.size() - 4, 4, ".log") == 0);
}

TEST_CASE("log_files: prune removes old day files and keeps today's") {
    const fs::path dir = makeTempDir("prune");
    touch(dir / ("proJV-" + dayString(0) + ".log"));
    touch(dir / ("proJV-" + dayString(2) + ".log"));
    touch(dir / ("proJV-" + dayString(10) + ".log"));

    const std::size_t removed = log_files::pruneOldLogs(dir.string(), 7);
    CHECK(removed == 1);
    CHECK(fs::exists(dir / ("proJV-" + dayString(0) + ".log")));
    CHECK(fs::exists(dir / ("proJV-" + dayString(2) + ".log")));
    CHECK_FALSE(fs::exists(dir / ("proJV-" + dayString(10) + ".log")));

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("log_files: prune never touches non-matching files") {
    const fs::path dir = makeTempDir("keep");
    const std::string old = dayString(30);
    touch(dir / ("proJV-" + old + ".log"));   // matches: deleted
    touch(dir / "proJV.log");                 // old fixed name
    touch(dir / "skill_metrics.log");
    touch(dir / ("crash-" + old + ".dmp"));
    touch(dir / ("proJV-" + old + ".log.bak"));
    touch(dir / ("archive-proJV-" + old + ".log"));
    touch(dir / "proJV-2026-13-40.log");      // month 13: unparseable, kept

    CHECK(log_files::pruneOldLogs(dir.string(), 7) == 1);
    CHECK(fs::exists(dir / "proJV.log"));
    CHECK(fs::exists(dir / "skill_metrics.log"));
    CHECK(fs::exists(dir / ("crash-" + old + ".dmp")));
    CHECK(fs::exists(dir / ("proJV-" + old + ".log.bak")));
    CHECK(fs::exists(dir / ("archive-proJV-" + old + ".log")));
    CHECK(fs::exists(dir / "proJV-2026-13-40.log"));

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("log_files: keepDays <= 0 disables pruning") {
    const fs::path dir = makeTempDir("off");
    touch(dir / ("proJV-" + dayString(100) + ".log"));
    CHECK(log_files::pruneOldLogs(dir.string(), 0) == 0);
    CHECK(fs::exists(dir / ("proJV-" + dayString(100) + ".log")));
    CHECK(log_files::pruneOldLogs(dir.string(), -3) == 0);

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("log_files: missing directory is not an error") {
    CHECK(log_files::pruneOldLogs("C:/definitely/not/here", 7) == 0);
    CHECK(log_files::pruneOldLogs("", 7) == 0);
}
