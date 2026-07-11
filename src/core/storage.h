#pragma once
#include <string>
#include <vector>
#include <memory>
#include "client/models.h"

namespace SQLite { class Database; }

// --- SQLite storage layer ------------------------------------------
// Dual-connection design: readDb_ (OPEN_READONLY) and writeDb_ (OPEN_READWRITE).
// Read methods always use readDb_; write methods always use writeDb_.
// With WAL mode, SQLite supports concurrent reads from one reader and one writer
// without locks. Callers MUST serialize writes via StorageWriteQueue or equivalent.
//
// Thread safety:
//   - Read: safe from any thread (WAL multi-reader)
//   - Write: must be called from one thread at a time (enforced by StorageWriteQueue)
class Storage {
public:
    Storage();
    ~Storage();

    // Non-copyable
    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;

    // -- Database lifecycle --------------------------------------
    // Create a new .db file -- opens writeDb_ (OPEN_READWRITE | OPEN_CREATE),
    // creates tables, then opens readDb_ (OPEN_READONLY).
    bool createDatabase(const std::string& path);

    // Open an existing .db file -- opens both connections.
    bool openDatabase(const std::string& path);

    // Close both connections.
    void closeDatabase();

    bool                 isOpen() const { return readDb_ != nullptr && writeDb_ != nullptr; }
    const std::string&   currentPath() const { return currentPath_; }

    // -- Message operations (core data) --------------------------
    // Insert a message and return its row id, or -1 on failure.
    // If the message has toolCalls, they are automatically written
    // to the tool_calls table.
    int64_t insertMessage(const Message& msg);

    // Query messages with id > sinceId (0 = all), ordered by id.
    std::vector<Message> queryMessages(int64_t sinceId = 0);

private:
    std::unique_ptr<SQLite::Database> readDb_;   // OPEN_READONLY -- any thread
    std::unique_ptr<SQLite::Database> writeDb_;  // OPEN_READWRITE -- writer thread only
    std::string                      currentPath_;

    // Create all required tables (idempotent) -- uses writeDb_
    bool ensureTables();

    // Write tool_calls rows for a message; returns false on failure
    bool insertToolCalls(int64_t messageId,
                         const std::vector<ToolCall>& calls);

    // Read tool_calls rows for a message
    std::vector<ToolCall> queryToolCalls(int64_t messageId);

    // Helper: open the read-only connection from currentPath_
    bool openReadDb();
};
