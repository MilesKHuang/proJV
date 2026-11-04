// Storage: the display-only `diff` column on `messages`.
//
// The diff a file-writing tool produces is persisted with its tool row so that
// reopening a session still shows the patch. It is display-only, so the two
// things worth locking are (a) it survives an insert/query round trip and (b) a
// database written before the column existed still opens and still reads.
#include "doctest.h"

#include "client/models.h"
#include "core/storage.h"

#include <SQLiteCpp/Database.h>

#include <filesystem>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace {

fs::path freshDbPath(const std::string& tag) {
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / ("projv_storage_" + tag);
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir / "session.pjv.db";
}

// The schema as it stood before `diff` was added. Spelled out by hand on
// purpose: copying CREATE TABLE from storage.cpp would silently stop testing the
// migration the day the two drift apart.
void createLegacyDb(const fs::path& path) {
    SQLite::Database db(path.string(), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    db.exec("CREATE TABLE messages ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "role TEXT NOT NULL, "
            "content TEXT, "
            "reasoning_content TEXT DEFAULT '', "
            "tool_call_id TEXT DEFAULT '', "
            "name TEXT DEFAULT '', "
            "token_count INTEGER DEFAULT 0, "
            "image_paths TEXT DEFAULT '', "
            "synthetic INTEGER DEFAULT 0, "
            "created_at TEXT NOT NULL DEFAULT (datetime('now')))");
    db.exec("CREATE TABLE tool_calls ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "message_id INTEGER NOT NULL, "
            "call_index INTEGER DEFAULT 0, "
            "tool_call_id TEXT NOT NULL, "
            "name TEXT NOT NULL, "
            "arguments TEXT, "
            "FOREIGN KEY (message_id) REFERENCES messages(id) ON DELETE CASCADE)");
    db.exec("INSERT INTO messages (role, content) VALUES ('user', 'written before the column')");
}

const char* kPatch = "--- a/a.txt\n+++ b/a.txt\n@@ -0,0 +1,1 @@\n+hello\n";

} // namespace

TEST_CASE("storage: the diff column survives an insert/query round trip") {
    const fs::path path = freshDbPath("roundtrip");

    Storage s;
    REQUIRE(s.createDatabase(path.string()));
    REQUIRE(s.isOpen());

    Message t = Message::Tool("call_1", "write_file", "Written 6 bytes to a.txt");
    t.diff = kPatch;
    const int64_t id = s.insertMessage(t);
    CHECK(id > 0);

    // A message without a diff keeps an empty column, not NULL.
    const int64_t plainId = s.insertMessage(Message::User("no diff here"));
    CHECK(plainId > id);

    const std::vector<Message> rows = s.queryMessages(0);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].role == "tool");
    CHECK(rows[0].name == "write_file");
    CHECK(rows[0].diff == kPatch);
    CHECK(rows[1].role == "user");
    CHECK(rows[1].diff.empty());

    // Reading back only the new rows works too.
    const std::vector<Message> newer = s.queryMessages(id);
    REQUIRE(newer.size() == 1);
    CHECK(newer[0].diff.empty());

    s.closeDatabase();
}

TEST_CASE("storage: a database without the diff column is migrated on open") {
    const fs::path path = freshDbPath("migrate");
    createLegacyDb(path);

    Storage s;
    REQUIRE(s.openDatabase(path.string()));
    REQUIRE(s.isOpen());

    // The pre-existing row is readable, with an empty diff.
    const std::vector<Message> rows = s.queryMessages(0);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].role == "user");
    CHECK(rows[0].content == "written before the column");
    CHECK(rows[0].diff.empty());

    // And the migrated column really accepts a write.
    Message t = Message::Tool("call_9", "edit_file", "Edited a.txt");
    t.diff = kPatch;
    CHECK(s.insertMessage(t) > 0);

    const std::vector<Message> after = s.queryMessages(rows[0].id);
    REQUIRE(after.size() == 1);
    CHECK(after[0].diff == kPatch);

    s.closeDatabase();
}
