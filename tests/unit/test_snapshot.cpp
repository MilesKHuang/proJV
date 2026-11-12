// snapshot: the engine behind /undo.
//
// Two layers are tested here: SHA-256 (content addressing depends on it, so a
// wrong digest silently corrupts every restore) and one end-to-end turn that
// modifies, adds and deletes a file and then undoes all three.
#include "doctest.h"

#include "core/snapshot.h"
#include "platform/isystem_util.h"
#include "tools/sha256.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

// snapshot::open() resolves projv_files through SystemUtil, which throws until
// Init() has run. main_tui does that at startup; a test binary has to do it here.
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

fs::path makeTempDir(const std::string& tag) {
    std::error_code ec;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path p =
        fs::temp_directory_path(ec) / ("projv_snaptest_" + tag + "_" + std::to_string(stamp));
    fs::create_directories(p, ec);
    return p;
}

void writeFile(const fs::path& p, const std::string& text) {
    std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
    ofs << text;
}

std::string readFile(const fs::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("sha256: FIPS 180-4 vectors") {
    CHECK(sha256::hexOf("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256::hexOf("abc") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256::hexOf("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("sha256: chunked input matches one-shot") {
    const std::string a(1000, 'x');
    sha256::Hasher h;
    for (int i = 0; i < 100; ++i)
        h.update(a.data() + i, 10);
    CHECK(h.hex() == sha256::hexOf(a));
}

TEST_CASE("snapshot: undo restores modified, added and deleted files") {
    const fs::path ws = makeTempDir("ws");
    writeFile(ws / "a.txt", "line1\n");
    writeFile(ws / "b.txt", "keep\n");

    std::string err;
    REQUIRE(snapshot::open(ws.string(), 7, 0, err));
    CHECK(snapshot::isAvailable());

    const int turn = snapshot::beginTurn("test_session", err);
    REQUIRE(turn >= 0);

    writeFile(ws / "a.txt", "line1 changed\n"); // modified
    writeFile(ws / "c.txt", "new\n");           // added
    fs::remove(ws / "b.txt");                   // deleted

    REQUIRE(snapshot::endTurn(turn, err));
    CHECK(snapshot::lastTurn("test_session") == turn);

    const snapshot::Report rep = snapshot::undoLast("test_session", false);
    CHECK(rep.ok);
    CHECK(rep.turnSeq == turn);
    CHECK(rep.restored.size() == 2); // a.txt + b.txt
    CHECK(rep.removed.size() == 1);  // c.txt

    CHECK(readFile(ws / "a.txt") == "line1\n");
    CHECK(readFile(ws / "b.txt") == "keep\n");
    CHECK_FALSE(fs::exists(ws / "c.txt"));

    // An undone turn cannot be undone twice.
    CHECK(snapshot::lastTurn("test_session") == -1);
    const snapshot::Report again = snapshot::undoLast("test_session", false);
    CHECK_FALSE(again.ok);
    CHECK_FALSE(again.error.empty());

    snapshot::close();
    std::error_code ec;
    fs::remove_all(ws, ec);
}

TEST_CASE("snapshot: a file edited after the turn needs force") {
    const fs::path ws = makeTempDir("force");
    writeFile(ws / "d.txt", "one\n");
    writeFile(ws / "e.txt", "keep\n");

    std::string err;
    REQUIRE(snapshot::open(ws.string(), 7, 0, err));
    const int turn = snapshot::beginTurn("force_session", err);
    REQUIRE(turn >= 0);
    writeFile(ws / "d.txt", "two\n");
    writeFile(ws / "e.txt", "changed\n");
    REQUIRE(snapshot::endTurn(turn, err));

    // The user edits one of the two files by hand after the turn ended.
    writeFile(ws / "d.txt", "three\n");

    const snapshot::Report guarded = snapshot::undoLast("force_session", false);
    CHECK_FALSE(guarded.ok);
    CHECK(guarded.needsForce);
    CHECK(readFile(ws / "d.txt") == "three\n"); // untouched
    // All-or-nothing: the file nobody touched must NOT have been restored,
    // otherwise the "not executed" message would be a lie.
    CHECK(readFile(ws / "e.txt") == "changed\n");

    const snapshot::Report forced = snapshot::undoLast("force_session", true);
    CHECK(forced.ok);
    CHECK(readFile(ws / "d.txt") == "one\n");
    CHECK(readFile(ws / "e.txt") == "keep\n");

    snapshot::close();
    std::error_code ec;
    fs::remove_all(ws, ec);
}
