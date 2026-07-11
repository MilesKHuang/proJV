#include "storage.h"
#include "debug_log.h"
#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>
#include <SQLiteCpp/Transaction.h>
#include <SQLiteCpp/Exception.h>

// -- Constructor / Destructor --------------------------------------

Storage::Storage() = default;

Storage::~Storage() {
    closeDatabase();
}

// -- Database lifecycle ---------------------------------------------

bool Storage::createDatabase(const std::string& path) {
    closeDatabase();
    try {
        // Step 1: Open writeDb_ with create flag, enable WAL, create tables
        writeDb_ = std::make_unique<SQLite::Database>(
            path,
            SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE,
            3000  // busy timeout 3s
        );
        currentPath_ = path;

        // Enable WAL mode + foreign keys (concurrent read performance)
        writeDb_->exec("PRAGMA journal_mode = WAL;");
        writeDb_->exec("PRAGMA foreign_keys = ON;");

        if (!ensureTables()) {
            debugLog("[Storage] createDatabase: ensureTables failed");
            writeDb_.reset();
            currentPath_.clear();
            return false;
        }

        // Step 2: Open read-only connection (WAL allows concurrent reads)
        if (!openReadDb()) {
            debugLog("[Storage] createDatabase: openReadDb failed");
            writeDb_.reset();
            currentPath_.clear();
            return false;
        }

        debugLogf("[Storage] Created database (dual): %s", path.c_str());
        return true;
    } catch (const SQLite::Exception& e) {
        debugLogf("[Storage] createDatabase exception: %s", e.what());
        writeDb_.reset();
        readDb_.reset();
        currentPath_.clear();
        return false;
    }
}

bool Storage::openDatabase(const std::string& path) {
    closeDatabase();
    try {
        // Open write connection
        writeDb_ = std::make_unique<SQLite::Database>(
            path,
            SQLite::OPEN_READWRITE,
            3000
        );
        currentPath_ = path;

        // Enable WAL mode for existing databases
        writeDb_->exec("PRAGMA journal_mode = WAL;");
        writeDb_->exec("PRAGMA foreign_keys = ON;");

        // 确保 schema 完整（兼容旧版本缺少 reasoning_content 列等）
        if (!ensureTables()) {
            debugLog("[Storage] openDatabase: ensureTables failed");
            writeDb_.reset();
            currentPath_.clear();
            return false;
        }

        // Open read-only connection
        if (!openReadDb()) {
            debugLog("[Storage] openDatabase: openReadDb failed");
            writeDb_.reset();
            currentPath_.clear();
            return false;
        }

        debugLogf("[Storage] Opened database (dual): %s", path.c_str());
        return true;
    } catch (const SQLite::Exception& e) {
        debugLogf("[Storage] openDatabase exception: %s", e.what());
        writeDb_.reset();
        readDb_.reset();
        currentPath_.clear();
        return false;
    }
}

bool Storage::openReadDb() {
    try {
        readDb_ = std::make_unique<SQLite::Database>(
            currentPath_,
            SQLite::OPEN_READONLY,
            3000
        );
        return true;
    } catch (const SQLite::Exception& e) {
        debugLogf("[Storage] openReadDb exception: %s", e.what());
        return false;
    }
}

void Storage::closeDatabase() {
    // Close read connection first, then write
    if (readDb_) {
        debugLogf("[Storage] Closed read connection: %s", currentPath_.c_str());
    }
    readDb_.reset();

    if (writeDb_) {
        debugLogf("[Storage] Closed write connection: %s", currentPath_.c_str());
    }
    writeDb_.reset();
    currentPath_.clear();
}

// -- Schema ---------------------------------------------------------

bool Storage::ensureTables() {
    try {
        writeDb_->exec(R"(
            CREATE TABLE IF NOT EXISTS messages (
                id                INTEGER PRIMARY KEY AUTOINCREMENT,
                role              TEXT NOT NULL,
                content           TEXT,
                reasoning_content TEXT DEFAULT '',
                tool_call_id      TEXT DEFAULT '',
                name              TEXT DEFAULT '',
                token_count       INTEGER DEFAULT 0,
                created_at        TEXT NOT NULL DEFAULT (datetime('now'))
            );
        )");
        // Migration: add reasoning_content column to existing databases
        try {
            writeDb_->exec("ALTER TABLE messages ADD COLUMN reasoning_content TEXT DEFAULT ''");
        } catch (const SQLite::Exception&) {
            // Column already exists — safe to ignore
        }
        writeDb_->exec(R"(
            CREATE TABLE IF NOT EXISTS tool_calls (
                id           INTEGER PRIMARY KEY AUTOINCREMENT,
                message_id   INTEGER NOT NULL,
                call_index   INTEGER DEFAULT 0,
                tool_call_id TEXT NOT NULL,
                name         TEXT NOT NULL,
                arguments    TEXT,
                FOREIGN KEY (message_id) REFERENCES messages(id) ON DELETE CASCADE
            );
        )");
        return true;
    } catch (const SQLite::Exception& e) {
        debugLogf("[Storage] ensureTables exception: %s", e.what());
        return false;
    }
}

// -- Message operations --------------------------------------------

static size_t estimateTokens(const std::string& text) {
    size_t t = 0;
    for (char c : text) {
        if ((unsigned char)c >= 0x80) t += 2;
        else t += 1;
    }
    return std::max<size_t>(1, t / 4);
}

int64_t Storage::insertMessage(const Message& msg) {
    if (!writeDb_) return -1;
    try {
        SQLite::Transaction transaction(*writeDb_, SQLite::TransactionBehavior::IMMEDIATE);

        size_t tokens = estimateTokens(msg.content);

        SQLite::Statement stmt(*writeDb_,
            "INSERT INTO messages (role, content, reasoning_content, tool_call_id, name, token_count) "
            "VALUES (?, ?, ?, ?, ?, ?)");
        stmt.bind(1, msg.role);
        stmt.bind(2, msg.content);
        stmt.bind(3, msg.reasoningContent);
        stmt.bind(4, msg.toolCallId);
        stmt.bind(5, msg.name);
        stmt.bind(6, static_cast<int64_t>(tokens));
        stmt.exec();

        int64_t messageId =
            writeDb_->execAndGet("SELECT last_insert_rowid()").getInt64();

        // Insert tool_calls for assistant messages
        if (!msg.toolCalls.empty()) {
            if (!insertToolCalls(messageId, msg.toolCalls)) {
                debugLog("[Storage] insertMessage: insertToolCalls failed");
            }
        }

        transaction.commit();

        debugLogf("[Storage] insertMessage: id=%lld role=%s (%zu chars)",
                  (long long)messageId, msg.role.c_str(), msg.content.size());
        return messageId;
    } catch (const SQLite::Exception& e) {
        debugLogf("[Storage] insertMessage exception: %s", e.what());
        return -1;
    }
}

std::vector<Message> Storage::queryMessages(int64_t sinceId) {
    std::vector<Message> result;
    if (!readDb_) return result;
    try {
        SQLite::Statement stmt(*readDb_,
            "SELECT id, role, content, reasoning_content, tool_call_id, name "
            "FROM messages WHERE id > ? ORDER BY id");
        stmt.bind(1, sinceId);

        while (stmt.executeStep()) {
            Message msg;
            int64_t msgId = stmt.getColumn(0).getInt64();
            msg.id              = msgId;
            msg.role            = stmt.getColumn(1).getString();
            msg.content         = stmt.getColumn(2).getString();
            msg.reasoningContent = stmt.getColumn(3).getString();
            msg.toolCallId      = stmt.getColumn(4).getString();
            msg.name            = stmt.getColumn(5).getString();

            // Load tool calls for assistant messages
            if (msg.role == "assistant") {
                msg.toolCalls = queryToolCalls(msgId);
            }

            result.push_back(std::move(msg));
        }
    } catch (const SQLite::Exception& e) {
        debugLogf("[Storage] queryMessages exception (SQLite): %s", e.what());
    } catch (const std::exception& e) {
        debugLogf("[Storage] queryMessages exception (std): %s", e.what());
    }
    return result;
}

// -- Tool calls operations -----------------------------------------

bool Storage::insertToolCalls(int64_t messageId,
                               const std::vector<ToolCall>& calls) {
    if (!writeDb_) return false;
    try {
        for (size_t i = 0; i < calls.size(); ++i) {
            SQLite::Statement stmt(*writeDb_,
                "INSERT INTO tool_calls (message_id, call_index, tool_call_id, name, arguments) "
                "VALUES (?, ?, ?, ?, ?)");
            stmt.bind(1, messageId);
            stmt.bind(2, static_cast<int64_t>(i));
            stmt.bind(3, calls[i].id);
            stmt.bind(4, calls[i].name);
            stmt.bind(5, calls[i].arguments);
            stmt.exec();
        }
        return true;
    } catch (const SQLite::Exception& e) {
        debugLogf("[Storage] insertToolCalls exception: %s", e.what());
        return false;
    }
}

std::vector<ToolCall> Storage::queryToolCalls(int64_t messageId) {
    std::vector<ToolCall> result;
    if (!readDb_) return result;
    try {
        SQLite::Statement stmt(*readDb_,
            "SELECT tool_call_id, name, arguments, call_index "
            "FROM tool_calls WHERE message_id = ? ORDER BY call_index");
        stmt.bind(1, messageId);

        while (stmt.executeStep()) {
            ToolCall tc;
            tc.id        = stmt.getColumn(0).getString();
            tc.name      = stmt.getColumn(1).getString();
            tc.arguments = stmt.getColumn(2).getString();
            tc.index     = stmt.getColumn(3).getInt();
            result.push_back(std::move(tc));
        }
    } catch (const SQLite::Exception& e) {
        debugLogf("[Storage] queryToolCalls exception (SQLite): %s", e.what());
    } catch (const std::exception& e) {
        debugLogf("[Storage] queryToolCalls exception (std): %s", e.what());
    }
    return result;
}