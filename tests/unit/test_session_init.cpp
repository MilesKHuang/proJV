// SessionRuntime::initialize(): the three things a "no behaviour change" commit
// deleted by accident (74f782d). Each CHECK below is one of them, so the failure
// says which invariant came back:
//
//   1. promptFiles_ = ensureDefaultPrompts()  -> a role prompt exists at all;
//   2. sessionsDir_ = getSessionsDir()        -> the DB lands under projv_files/
//                                                sessions and listSessions() is not
//                                                empty (F4 / Open read that list);
//   3. the role prompt as the FIRST persisted message -> the fixed prefix is exactly
//      three system messages (role, environment, project), which is the index
//      buildChatRequest() and Session::applyCompaction() both rely on.
//
// Without (1) the model runs with an empty system prompt; without (2) a new
// conversation is created at "/session_<ts>.db" (the root of the current drive on
// Windows); without (3) the capability map and the compaction summary are inserted
// one slot early. None of those throw, so only a test catches them.
#include "doctest.h"

#include "core/session_runtime.h"
#include "core/snapshot.h"
#include "platform/isystem_util.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// getSessionsDir()/getProjvDir() go through SystemUtil, which throws until Init().
// main_tui does this at startup; a test binary has to do it here.
struct EnsureSystemUtil {
    EnsureSystemUtil() {
        try {
            SystemUtil::Init(CreateSystemUtil());
        } catch (...) {
            // already initialised by another translation unit -- fine
        }
    }
};
EnsureSystemUtil g_ensureSystemUtil;

fs::path makeTempWorkspace(const std::string& tag) {
    std::error_code ec;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path p = fs::temp_directory_path(ec) / ("projv_init_" + tag + "_" + std::to_string(stamp));
    fs::create_directories(p, ec);
    return p;
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// The snapshot store is a process-wide singleton (see C1 in the regression note),
// and an open store keeps pointing at a workspace that the test then deletes. Every
// case closes it, so one case cannot decide the next one's workspace.
struct SnapshotGuard {
    ~SnapshotGuard() { snapshot::close(); }
};

} // namespace

TEST_CASE("runtime initialize: role prompt list is populated") {
    SnapshotGuard snap;
    const fs::path ws = makeTempWorkspace("prompts");

    SessionRuntime rt;
    rt.setWorkspace(ws.string());
    REQUIRE(rt.initialize());

    // (1) A fresh install has at least the seeded role, and compactor.md is not a
    // role: it is the /compress prompt and is filtered out of the picker.
    CHECK_FALSE(rt.promptFiles().empty());
    for (const std::string& f : rt.promptFiles()) CHECK(f != "compactor.md");
    CHECK(rt.activePromptIndex() >= 0);
    CHECK(rt.activePromptIndex() < static_cast<int>(rt.promptFiles().size()));

    rt.shutdown();
}

TEST_CASE("runtime initialize: the fixed prefix is three system messages") {
    SnapshotGuard snap;
    const fs::path ws = makeTempWorkspace("prefix");

    SessionRuntime rt;
    rt.setWorkspace(ws.string());
    REQUIRE(rt.initialize());
    REQUIRE(rt.agent() != nullptr);

    const std::vector<Message> msgs = rt.agent()->getSession().getContextMessages();
    // (3) P0 = role, P1 = [ENVIRONMENT], P2 = [PROJECT]. A missing P0 leaves two.
    REQUIRE(msgs.size() >= 3);
    CHECK(msgs[0].role == "system");
    CHECK(msgs[1].role == "system");
    CHECK(msgs[2].role == "system");
    CHECK(msgs[0].content.find("proJV") != std::string::npos); // the role seed
    CHECK(contains(msgs[1].content, "[ENVIRONMENT]"));
    CHECK(contains(msgs[2].content, "[PROJECT"));

    rt.shutdown();
}

TEST_CASE("runtime initialize: a new session is stored under projv_files/sessions") {
    SnapshotGuard snap;
    const fs::path ws = makeTempWorkspace("sessions");

    SessionRuntime rt;
    rt.setWorkspace(ws.string());
    REQUIRE(rt.initialize());

    // (2) Nothing is on disk until the first message: submit() is what calls
    // ensureStorage(), which is where a database is lazily created. Before the fix
    // sessionsDir_ was empty, so the path was "/session_<ts>.db" and this list
    // stayed empty -- that is the blank F4 / Open dialog.
    rt.submit("hello");
    const std::vector<std::string> sessions = rt.listSessions();
    REQUIRE_FALSE(sessions.empty());

    bool underSessionsDir = false;
    for (const std::string& path : sessions) {
        if (!contains(path, ".db")) continue;
        // Both separators accepted: the app runs on Windows and Linux.
        if (contains(path, "/sessions/") || contains(path, "\\sessions\\")) underSessionsDir = true;
    }
    CHECK(underSessionsDir);

    rt.shutdown();
}

TEST_CASE("runtime initialize: two sessions both get their own prompt list") {
    // Guards the studio's "+ New" path: initialize() has to be repeatable, and each
    // runtime keeps its own workspace (a second one must not inherit the first's).
    SnapshotGuard snap;
    const fs::path ws1 = makeTempWorkspace("two_a");
    const fs::path ws2 = makeTempWorkspace("two_b");

    SessionRuntime a;
    a.setWorkspace(ws1.string());
    REQUIRE(a.initialize());

    SessionRuntime b;
    b.setWorkspace(ws2.string());
    REQUIRE(b.initialize());

    CHECK_FALSE(a.promptFiles().empty());
    CHECK_FALSE(b.promptFiles().empty());
    CHECK(a.workspace() != b.workspace());

    a.shutdown();
    b.shutdown();
}
