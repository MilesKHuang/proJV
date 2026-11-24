#include "snapshot.h"

#include "config.h"
#include "debug_log.h"
#include "tools/sha256.h"

#include <SQLiteCpp/SQLiteCpp.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace snapshot {
namespace {

// One file over this is not snapshotted: a source tree's business is in text,
// and a 200 MB asset would be copied on every edit.
constexpr long long kMaxFileBytes = 256 * 1024;

// A turn that never ends (cancelled before its endTurn) must not grow the
// baseline map for ever. Eight open turns is far more than the studio can have
// in flight; the ninth drops the oldest, which only costs that turn its diff.
constexpr size_t kMaxOpenTurns = 8;

// Directories that never belong to a turn's work. Mirrors the lists in
// file_tool.cpp / project_context.cpp; the snapshot copy additionally excludes
// the store itself and the whole projv_files tree (see scan()).
bool isSkippedDir(const std::string& name) {
    static const char* k[] = {".git",     ".svn",        ".hg",           ".vs",         ".vscode",
                              ".idea",    "build",       "Debug",         "Release",     "x64",
                              "x86",      "bin",         "bin64",         "obj",         "out",
                              "target",   "dist",        "node_modules",  "packages",    "vendor",
                              "external", "__pycache__", ".pytest_cache", ".mypy_cache", ".cache",
                              "coverage", "venv",        ".venv"};
    for (const char* s : k)
        if (name == s) return true;
    return false;
}

struct Meta {
    // int64_t, not long long: SQLiteCpp's bind() has int32/uint32/int64/double
    // overloads, and on Linux int64_t is `long`, so a `long long` argument is
    // ambiguous there (it compiles on MSVC, where the two are the same type).
    std::int64_t mtime = 0;
    std::int64_t size = -1;
    bool operator==(const Meta& o) const { return mtime == o.mtime && size == o.size; }
};

// One workspace's store. Everything that used to be one process-wide `State`
// lives here, and there is one WorkspaceStore per canonical workspace path.
struct WorkspaceStore {
    bool open = false;
    std::string reason;
    fs::path projvDir;
    fs::path workspace;
    fs::path root;    // <projv>/snapshots/<hash>
    fs::path blobDir; // <root>/blobs
    std::string dbPath;
    std::unique_ptr<SQLite::Database> db;
    // The workspace as each open turn found it, keyed by turn seq. One slot was
    // not enough: two panes may run a turn at the same time, and the second
    // beginTurn() would overwrite the first pane's "before" state, so the first
    // endTurn() would diff against a picture it never saw.
    std::map<int, std::map<std::string, Meta>> begin;
    std::mutex mtx;
};

std::mutex g_registryMutex;
std::vector<std::shared_ptr<WorkspaceStore>> g_stores; // handle == index
std::map<std::string, Store> g_handles;       // canonical workspace -> handle

// A handle outlives the call: close() is a test seam, and tests are single
// threaded. Returning the shared_ptr keeps the store alive while a call is in
// flight even if close() runs.
std::shared_ptr<WorkspaceStore> storeAt(Store s) {
    std::lock_guard<std::mutex> lk(g_registryMutex);
    if (s < 0 || (size_t)s >= g_stores.size()) return nullptr;
    return g_stores[(size_t)s];
}

std::int64_t stamp(const fs::file_time_type& t) {
    return (std::int64_t)t.time_since_epoch().count();
}

std::string nowIso() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

bool readFile(const fs::path& p, std::string& out) {
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) return false;
    out.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
    return true;
}

bool looksBinary(const std::string& s) {
    const size_t n = std::min<size_t>(s.size(), 8192);
    for (size_t i = 0; i < n; ++i)
        if (s[i] == '\0') return true;
    return false;
}

std::string lower(const std::string& s) {
    std::string o = s;
    for (char& c : o)
        c = (char)std::tolower((unsigned char)c);
    return o;
}

bool under(const fs::path& p, const fs::path& root) {
    if (root.empty()) return false;
    const std::string a = lower(p.lexically_normal().string());
    const std::string b = lower(root.lexically_normal().string());
    if (a.size() <= b.size()) return false;
    if (a.compare(0, b.size(), b) != 0) return false;
    const char c = a[b.size()];
    return c == '/' || c == '\\';
}

// Relative path -> mtime/size. Directories, projv_files and the usual build
// artefact directories never enter the map.
void scan(WorkspaceStore& st, std::map<std::string, Meta>& out) {
    out.clear();
    std::error_code ec;
    fs::recursive_directory_iterator it(st.workspace, fs::directory_options::skip_permission_denied,
                                        ec);
    if (ec) return;
    const fs::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        const fs::directory_entry& e = *it;
        if (under(e.path(), st.projvDir)) {
            it.disable_recursion_pending();
            continue;
        }
        if (e.is_directory(ec)) {
            if (isSkippedDir(e.path().filename().string())) it.disable_recursion_pending();
            continue;
        }
        if (!e.is_regular_file(ec)) continue;
        const std::string rel = fs::relative(e.path(), st.workspace, ec).generic_string();
        if (ec || rel.empty()) continue;
        Meta m;
        m.mtime = stamp(e.last_write_time(ec));
        if (ec) continue;
        m.size = (std::int64_t)e.file_size(ec);
        if (ec) continue;
        out[rel] = m;
    }
}

fs::path blobPath(WorkspaceStore& st, const std::string& hash) {
    return st.blobDir / hash.substr(0, 2) / hash;
}

bool haveBlob(WorkspaceStore& st, const std::string& hash) {
    if (hash.empty()) return false;
    std::error_code ec;
    return fs::exists(blobPath(st, hash), ec) && !ec;
}

// Write through a temporary file: a crash mid-write must not leave a truncated
// blob behind a valid hash.
bool storeBlob(WorkspaceStore& st, const std::string& content, const std::string& hash) {
    if (haveBlob(st, hash)) return true;
    std::error_code ec;
    const fs::path target = blobPath(st, hash);
    fs::create_directories(target.parent_path(), ec);
    if (ec) return false;
    const fs::path tmp = target.string() + ".tmp";
    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs) return false;
        ofs.write(content.data(), (std::streamsize)content.size());
        if (!ofs) return false;
    }
    fs::rename(tmp, target, ec);
    if (ec) {
        std::error_code rmEc;
        fs::remove(tmp, rmEc);
        return false;
    }
    return true;
}

bool readBlob(WorkspaceStore& st, const std::string& hash, std::string& out) {
    if (hash.empty()) return false;
    return readFile(blobPath(st, hash), out);
}
bool schema(WorkspaceStore& st) {
    try {
        st.db->exec("CREATE TABLE IF NOT EXISTS turns("
                    "seq INTEGER PRIMARY KEY AUTOINCREMENT,"
                    "started_at TEXT NOT NULL,"
                    "ended_at TEXT,"
                    "session_id TEXT NOT NULL,"
                    "undone INTEGER NOT NULL DEFAULT 0);");
        st.db->exec("CREATE TABLE IF NOT EXISTS changes("
                    "turn_seq INTEGER NOT NULL,"
                    "path TEXT NOT NULL,"
                    "op TEXT NOT NULL,"
                    "old_hash TEXT,"
                    "new_hash TEXT,"
                    "PRIMARY KEY(turn_seq, path));");
        st.db->exec("CREATE TABLE IF NOT EXISTS files("
                    "path TEXT PRIMARY KEY,"
                    "mtime INTEGER NOT NULL,"
                    "size INTEGER NOT NULL,"
                    "hash TEXT NOT NULL);");
        // Who produced the content that is on disk now. The file system cannot say
        // who wrote a file, but the turn whose end saw exactly this (mtime, size)
        // can: a turn end in another session then leaves the file alone instead of
        // recording somebody else's work as its own. mtime = -1 means "deleted".
        // A table of its own, not a column: CREATE TABLE IF NOT EXISTS adds it to
        // an existing store without a migration.
        st.db->exec("CREATE TABLE IF NOT EXISTS claims("
                    "path TEXT PRIMARY KEY,"
                    "mtime INTEGER NOT NULL,"
                    "size INTEGER NOT NULL,"
                    "session_id TEXT NOT NULL,"
                    "turn_seq INTEGER NOT NULL);");
        st.db->exec("CREATE INDEX IF NOT EXISTS idx_turns_session ON turns(session_id, seq);");
        return true;
    } catch (const std::exception& e) {
        debugLogErrf("[snapshot] schema failed: %s", e.what());
        return false;
    }
}

unsigned long long workspaceBytes(WorkspaceStore& st) {
    std::map<std::string, Meta> s;
    scan(st, s);
    unsigned long long total = 0;
    for (const auto& [rel, m] : s)
        if (m.size > 0) total += (unsigned long long)m.size;
    return total;
}

// Remove stores nobody touched for keepDays. Only directories that contain an
// index.db are considered ours: anything else in snapshots/ is left alone. A root
// that belongs to a store open right now is never removed -- another pane is
// using it.
void pruneStale(int keepDays, const fs::path& snapshotsRoot,
                const std::vector<fs::path>& inUse) {
    if (keepDays <= 0) return;
    std::error_code ec;
    const auto cutoff = fs::file_time_type::clock::now() - std::chrono::hours(24 * keepDays);
    for (const auto& e : fs::directory_iterator(snapshotsRoot, ec)) {
        if (ec) break;
        if (!e.is_directory(ec)) continue;
        bool live = false;
        for (const fs::path& p : inUse) {
            if (p == e.path()) {
                live = true;
                break;
            }
        }
        if (live) continue;
        const fs::path idx = e.path() / "index.db";
        if (!fs::exists(idx, ec)) continue;
        const auto touched = fs::last_write_time(idx, ec);
        if (ec) continue;
        if (touched < cutoff) {
            fs::remove_all(e.path(), ec);
            if (!ec) debugLogInfof("[snapshot] pruned stale store %s", e.path().string().c_str());
        }
    }
}

// First open reads and hashes everything once. Without this, the very first
// edit of a file could not be undone: the old content has to exist somewhere.
bool takeBaseline(WorkspaceStore& st, std::string& err) {
    try {
        SQLite::Statement count(*st.db, "SELECT COUNT(*) FROM files");
        if (count.executeStep() && count.getColumn(0).getInt() > 0) return true;
    } catch (const std::exception&) {
    }

    std::map<std::string, Meta> snap;
    scan(st, snap);
    size_t stored = 0;
    try {
        SQLite::Transaction tr(*st.db);
        SQLite::Statement ins(
            *st.db, "INSERT OR REPLACE INTO files(path,mtime,size,hash) VALUES(?,?,?,?)");
        for (const auto& [rel, m] : snap) {
            if (m.size < 0 || m.size > kMaxFileBytes) continue;
            std::string content;
            if (!readFile(st.workspace / rel, content)) continue;
            if ((long long)content.size() > kMaxFileBytes || looksBinary(content)) continue;
            const std::string hash = sha256::hexOf(content);
            if (!storeBlob(st, content, hash)) continue;
            ins.bind(1, rel);
            ins.bind(2, m.mtime);
            ins.bind(3, m.size);
            ins.bind(4, hash);
            ins.exec();
            ins.reset();
            ins.clearBindings();
            ++stored;
        }
        tr.commit();
    } catch (const std::exception& e) {
        err = std::string("baseline failed: ") + e.what();
        debugLogErrf("[snapshot] %s", err.c_str());
        return false;
    }
    debugLogInfof("[snapshot] baseline: %zu file(s), %zu tracked", snap.size(), stored);
    return true;
}

std::string hashOfFile(WorkspaceStore& st, const std::string& rel, bool& ok, bool& torn) {
    ok = false;
    torn = false;
    const fs::path p = st.workspace / rel;
    std::error_code ec;
    const long long a = (long long)fs::file_size(p, ec);
    if (ec) return std::string();
    std::string content;
    if (!readFile(p, content)) return std::string();
    const long long b = (long long)fs::file_size(p, ec);
    if (ec) return std::string();
    if (a != (long long)content.size() || a != b) {
        torn = true; // somebody is writing it right now
        return std::string();
    }
    if ((long long)content.size() > kMaxFileBytes || looksBinary(content)) return std::string();
    const std::string hash = sha256::hexOf(content);
    // Store the blob now: it is the "after" state, and it becomes the "before"
    // state of a later turn.
    if (!storeBlob(st, content, hash)) return std::string();
    ok = true;
    return hash;
}
bool setEnded(WorkspaceStore& st, int seq) {
    try {
        SQLite::Statement up(*st.db, "UPDATE turns SET ended_at=? WHERE seq=?");
        up.bind(1, nowIso());
        up.bind(2, seq);
        up.exec();
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void dropTurn(WorkspaceStore& st, int seq) {
    try {
        SQLite::Statement del(*st.db, "DELETE FROM turns WHERE seq=?");
        del.bind(1, seq);
        del.exec();
    } catch (const std::exception&) {
    }
}

std::string storedHash(WorkspaceStore& st, const std::string& rel) {
    try {
        SQLite::Statement q(*st.db, "SELECT hash FROM files WHERE path=?");
        q.bind(1, rel);
        if (q.executeStep()) return q.getColumn(0).getString();
    } catch (const std::exception&) {
    }
    return std::string();
}

void upsertFile(WorkspaceStore& st, const std::string& rel, const Meta& m, const std::string& hash) {
    try {
        SQLite::Statement up(
            *st.db, "INSERT OR REPLACE INTO files(path,mtime,size,hash) VALUES(?,?,?,?)");
        up.bind(1, rel);
        up.bind(2, m.mtime);
        up.bind(3, m.size);
        up.bind(4, hash);
        up.exec();
    } catch (const std::exception&) {
    }
}

void forgetFile(WorkspaceStore& st, const std::string& rel) {
    try {
        SQLite::Statement del(*st.db, "DELETE FROM files WHERE path=?");
        del.bind(1, rel);
        del.exec();
    } catch (const std::exception&) {
    }
}

void recordChange(WorkspaceStore& st, int seq, const std::string& rel, const char* op,
                  const std::string& oldHash, const std::string& newHash) {
    try {
        SQLite::Statement ins(*st.db,
                              "INSERT OR REPLACE INTO changes(turn_seq,path,op,old_hash,new_hash) "
                              "VALUES(?,?,?,?,?)");
        ins.bind(1, seq);
        ins.bind(2, rel);
        ins.bind(3, std::string(op));
        if (oldHash.empty())
            ins.bind(4);
        else
            ins.bind(4, oldHash);
        if (newHash.empty())
            ins.bind(5);
        else
            ins.bind(5, newHash);
        ins.exec();
    } catch (const std::exception&) {
    }
}

// Hash of what is on disk right now, without storing a blob.
std::string currentHash(WorkspaceStore& st, const std::string& rel) {
    std::string content;
    if (!readFile(st.workspace / rel, content)) return std::string();
    if ((long long)content.size() > kMaxFileBytes || looksBinary(content)) return std::string();
    return sha256::hexOf(content);
}
bool writeRestored(WorkspaceStore& st, const std::string& rel, const std::string& content) {
    const fs::path target = st.workspace / rel;
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    if (ec) return false;
    const fs::path tmp = target.string() + ".projv-undo.tmp";
    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs) return false;
        ofs.write(content.data(), (std::streamsize)content.size());
        if (!ofs) return false;
    }
    fs::rename(tmp, target, ec);
    if (ec) {
        std::error_code rmEc;
        fs::remove(tmp, rmEc);
        return false;
    }
    return true;
}

struct Change {
    std::string path;
    std::string op;
    std::string oldHash;
    std::string newHash;
};

std::vector<Change> changesOf(WorkspaceStore& st, int seq) {
    std::vector<Change> out;
    try {
        SQLite::Statement q(*st.db,
                            "SELECT path,op,old_hash,new_hash FROM changes WHERE turn_seq=? "
                            "ORDER BY path");
        q.bind(1, seq);
        while (q.executeStep()) {
            Change c;
            c.path = q.getColumn(0).getString();
            c.op = q.getColumn(1).getString();
            c.oldHash = q.getColumn(2).isNull() ? std::string() : q.getColumn(2).getString();
            c.newHash = q.getColumn(3).isNull() ? std::string() : q.getColumn(3).getString();
            out.push_back(std::move(c));
        }
    } catch (const std::exception& e) {
        debugLogErrf("[snapshot] reading changes failed: %s", e.what());
    }
    return out;
}

// Is the state (mtime, size) on this path somebody else's work? -1/-1 is a
// deletion. Without this, a turn that runs while another pane edits would claim
// that pane's files, and /undo would offer to revert work this session never did.
bool claimedByOther(WorkspaceStore& st, const std::string& rel, std::int64_t mtime, std::int64_t size,
                    const std::string& sessionId) {
    try {
        SQLite::Statement q(*st.db, "SELECT session_id,mtime,size FROM claims WHERE path=?");
        q.bind(1, rel);
        if (q.executeStep()) {
            const std::string owner = q.getColumn(0).getString();
            const std::int64_t m = q.getColumn(1).getInt64();
            const std::int64_t s = q.getColumn(2).getInt64();
            return owner != sessionId && m == mtime && s == size;
        }
    } catch (const std::exception&) {
    }
    return false;
}

void claim(WorkspaceStore& st, const std::string& rel, std::int64_t mtime, std::int64_t size,
           const std::string& sessionId, int seq) {
    try {
        SQLite::Statement up(*st.db,
                             "INSERT OR REPLACE INTO claims(path,mtime,size,session_id,turn_seq) "
                             "VALUES(?,?,?,?,?)");
        up.bind(1, rel);
        up.bind(2, mtime);
        up.bind(3, size);
        up.bind(4, sessionId);
        up.bind(5, seq);
        up.exec();
    } catch (const std::exception&) {
    }
}

std::string turnSession(WorkspaceStore& st, int seq) {
    try {
        SQLite::Statement q(*st.db, "SELECT session_id FROM turns WHERE seq=?");
        q.bind(1, seq);
        if (q.executeStep()) return q.getColumn(0).getString();
    } catch (const std::exception&) {
    }
    return std::string();
}

// Undo one recorded turn. `force` skips the "file changed after the turn"
// check the first time round, so the caller can ask the user.
void applyTurn(WorkspaceStore& st, int seq, bool force, Report& r) {
    const std::vector<Change> changes = changesOf(st, seq);

    // Check everything before writing anything. A half-applied turn that then
    // reports "not executed" is worse than no undo at all: the message would be
    // a lie, and the user believes the workspace was left alone.
    if (!force) {
        for (const Change& c : changes) {
            bool untouched = false;
            if (c.op == "A") {
                const std::string now = currentHash(st, c.path);
                untouched = now.empty() || now == c.newHash;
            } else {
                const std::string now = currentHash(st, c.path);
                untouched = (c.op == "D") ? now.empty() : (now == c.newHash);
            }
            if (!untouched) {
                r.needsForce = true;
                r.skipped.push_back(c.path);
            }
        }
        if (r.needsForce) {
            r.turnSeq = seq;
            return; // nothing was touched
        }
    }

    for (const Change& c : changes) {
        if (c.op == "A") {
            std::error_code ec;
            if (fs::remove(st.workspace / c.path, ec) && !ec) {
                r.removed.push_back(c.path);
                forgetFile(st, c.path);
            }
            continue;
        }

        // M and D both restore the old content.
        if (c.oldHash.empty() || !haveBlob(st, c.oldHash)) {
            r.skipped.push_back(c.path);
            continue;
        }
        std::string content;
        if (!readBlob(st, c.oldHash, content)) {
            r.skipped.push_back(c.path);
            continue;
        }
        if (writeRestored(st, c.path, content)) {
            r.restored.push_back(c.path);
            std::error_code ec;
            Meta m;
            m.size = (std::int64_t)content.size();
            m.mtime = stamp(fs::last_write_time(st.workspace / c.path, ec));
            if (!ec) upsertFile(st, c.path, m, c.oldHash);
        } else {
            r.skipped.push_back(c.path);
        }
    }
    r.turnSeq = seq;
}

void markUndone(WorkspaceStore& st, int seq) {
    try {
        SQLite::Statement up(*st.db, "UPDATE turns SET undone=1 WHERE seq=?");
        up.bind(1, seq);
        up.exec();
    } catch (const std::exception&) {
    }
}

} // namespace
Store open(const std::string& workspace, int keepDays, int maxWorkspaceGb, std::string& err) {
    err.clear();
    if (workspace.empty()) {
        err = "no workspace";
        return kNoStore;
    }

    std::error_code ec;
    fs::path ws = fs::weakly_canonical(workspace, ec);
    if (ec) ws = fs::absolute(workspace, ec);
    if (ec) {
        err = "cannot resolve the workspace path";
        return kNoStore;
    }
    const std::string key = lower(ws.string());

    // Held for the whole body: opening happens once per workspace and a second
    // pane must not start a second baseline on the same folder -- it gets the
    // handle that is already there, which is the whole point of the registry.
    std::lock_guard<std::mutex> reg(g_registryMutex);
    if (const auto it = g_handles.find(key); it != g_handles.end()) {
        err = g_stores[(size_t)it->second]->reason;
        return it->second;
    }

    auto st = std::make_shared<WorkspaceStore>();
    const Store handle = (Store)g_stores.size();
    g_stores.push_back(st);
    g_handles[key] = handle;

    st->workspace = ws;
    st->projvDir = getProjvDir();
    const std::string wsHash = sha256::hexOf(key);
    st->root = fs::path(st->projvDir) / "snapshots" / wsHash.substr(0, 16);
    st->blobDir = st->root / "blobs";
    fs::create_directories(st->blobDir, ec);
    if (ec) {
        st->reason = "cannot create " + st->blobDir.string();
        debugLogErrf("[snapshot] %s", st->reason.c_str());
        err = st->reason;
        return handle;
    }

    if (maxWorkspaceGb > 0) {
        const unsigned long long limit =
            (unsigned long long)maxWorkspaceGb * 1024ull * 1024ull * 1024ull;
        const unsigned long long bytes = workspaceBytes(*st);
        if (bytes > limit) {
            char msg[256];
            std::snprintf(msg, sizeof(msg),
                          "workspace is %.1f GB, over the %d GB snapshot limit; "
                          "undo is disabled (raise snapshots_max_workspace_gb in config.toml)",
                          (double)bytes / (1024.0 * 1024.0 * 1024.0), maxWorkspaceGb);
            st->reason = msg;
            err = st->reason;
            debugLogWarnf("[snapshot] %s", st->reason.c_str());
            return handle;
        }
    }

    std::vector<fs::path> inUse;
    inUse.reserve(g_stores.size());
    for (const auto& other : g_stores) inUse.push_back(other->root);
    pruneStale(keepDays, fs::path(st->projvDir) / "snapshots", inUse);

    st->dbPath = (st->root / "index.db").string();
    try {
        st->db = std::make_unique<SQLite::Database>(st->dbPath,
                                                    SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        st->db->exec("PRAGMA journal_mode=WAL;");
        st->db->exec("PRAGMA synchronous=NORMAL;");
    } catch (const std::exception& e) {
        st->reason = std::string("cannot open ") + st->dbPath + ": " + e.what();
        debugLogErrf("[snapshot] %s", st->reason.c_str());
        err = st->reason;
        st->db.reset();
        return handle;
    }
    if (!schema(*st)) {
        st->reason = "cannot create the snapshot tables";
        debugLogErrf("[snapshot] %s (%s)", st->reason.c_str(), st->dbPath.c_str());
        err = st->reason;
        return handle;
    }
    if (!takeBaseline(*st, err)) {
        st->reason = err;
        return handle;
    }
    st->open = true;
    debugLogInfof("[snapshot] open %s (session store %s)", st->workspace.string().c_str(),
                  st->root.string().c_str());
    return handle;
}

bool isAvailable(Store s) {
    auto st = storeAt(s);
    if (!st) return false;
    std::lock_guard<std::mutex> lk(st->mtx);
    return st->open;
}

int beginTurn(Store s, const std::string& sessionId, std::string& err) {
    err.clear();
    auto st = storeAt(s);
    if (!st) {
        err = "snapshots are not available";
        return -1;
    }
    std::lock_guard<std::mutex> lk(st->mtx);
    if (!st->open) {
        err = st->reason.empty() ? "snapshots are not available" : st->reason;
        return -1;
    }

    std::map<std::string, Meta> before;
    scan(*st, before);
    try {
        SQLite::Statement ins(*st->db, "INSERT INTO turns(started_at,session_id) VALUES(?,?)");
        ins.bind(1, nowIso());
        ins.bind(2, sessionId.empty() ? std::string("unknown") : sessionId);
        ins.exec();
        const int seq = (int)st->db->getLastInsertRowid();
        st->begin[seq] = std::move(before);
        while (st->begin.size() > kMaxOpenTurns) {
            debugLogWarnf("[snapshot] turn %d never ended; dropping its baseline",
                          st->begin.begin()->first);
            st->begin.erase(st->begin.begin());
        }
        return seq;
    } catch (const std::exception& e) {
        err = std::string("cannot start a snapshot turn: ") + e.what();
        return -1;
    }
}
bool endTurn(Store s, int turnSeq, std::string& err) {
    err.clear();
    auto st = storeAt(s);
    if (!st) {
        err = "snapshots are not available";
        return false;
    }
    std::lock_guard<std::mutex> lk(st->mtx);
    if (!st->open || turnSeq < 0) return false;

    // Its own baseline, not "whatever the last beginTurn left": two panes can run
    // a turn at the same time.
    const auto mine = st->begin.find(turnSeq);
    if (mine == st->begin.end()) return false;
    std::map<std::string, Meta> before = std::move(mine->second);
    st->begin.erase(mine);

    const std::string sessionId = turnSession(*st, turnSeq);
    std::map<std::string, Meta> after;
    scan(*st, after);

    size_t recorded = 0;
    for (const auto& [rel, m] : after) {
        const auto it = before.find(rel);
        const bool added = (it == before.end());
        if (!added && it->second == m) continue;
        // The file on disk is exactly what another session's turn left behind: it
        // is their work, not this turn's.
        if (claimedByOther(*st, rel, m.mtime, m.size, sessionId)) continue;

        bool ok = false, torn = false;
        const std::string hash = hashOfFile(*st, rel, ok, torn);
        if (!ok) continue; // binary, too large, unreadable or being written
        const std::string oldHash = added ? std::string() : storedHash(*st, rel);
        recordChange(*st, turnSeq, rel, (added && oldHash.empty()) ? "A" : "M", oldHash, hash);
        upsertFile(*st, rel, m, hash);
        claim(*st, rel, m.mtime, m.size, sessionId, turnSeq);
        ++recorded;
    }
    for (const auto& [rel, m] : before) {
        if (after.count(rel)) continue;
        if (claimedByOther(*st, rel, -1, -1, sessionId)) continue; // another session deleted it
        const std::string oldHash = storedHash(*st, rel);
        if (oldHash.empty()) continue;
        recordChange(*st, turnSeq, rel, "D", oldHash, std::string());
        forgetFile(*st, rel);
        claim(*st, rel, -1, -1, sessionId, turnSeq);
        ++recorded;
    }
    if (recorded == 0) {
        // Nothing moved: do not leave an empty turn behind, or /undo would
        // spend a click on it.
        dropTurn(*st, turnSeq);
        return true;
    }
    if (!setEnded(*st, turnSeq)) {
        err = "cannot close the snapshot turn";
        return false;
    }
    debugLogInfof("[snapshot] turn %d: %zu change(s)", turnSeq, recorded);
    return true;
}

int lastTurn(Store s, const std::string& sessionId) {
    auto st = storeAt(s);
    if (!st) return -1;
    std::lock_guard<std::mutex> lk(st->mtx);
    if (!st->open) return -1;
    try {
        SQLite::Statement q(
            *st->db, "SELECT MAX(seq) FROM turns WHERE session_id=? AND ended_at IS NOT NULL "
                     "AND undone=0");
        q.bind(1, sessionId.empty() ? std::string("unknown") : sessionId);
        if (q.executeStep() && !q.getColumn(0).isNull()) return q.getColumn(0).getInt();
    } catch (const std::exception&) {
    }
    return -1;
}

Report undoLast(Store s, const std::string& sessionId, bool force) {
    Report r;
    auto st = storeAt(s);
    if (!st) {
        r.error = "snapshots are not available";
        return r;
    }
    std::lock_guard<std::mutex> lk(st->mtx);
    if (!st->open) {
        r.error = st->reason.empty() ? "snapshots are not available" : st->reason;
        return r;
    }
    int seq = -1;
    try {
        SQLite::Statement q(
            *st->db, "SELECT MAX(seq) FROM turns WHERE session_id=? AND ended_at IS NOT NULL "
                     "AND undone=0");
        q.bind(1, sessionId.empty() ? std::string("unknown") : sessionId);
        if (q.executeStep() && !q.getColumn(0).isNull()) seq = q.getColumn(0).getInt();
    } catch (const std::exception& e) {
        r.error = e.what();
        return r;
    }
    if (seq < 0) {
        r.error = "no turn to undo in this session";
        return r;
    }
    applyTurn(*st, seq, force, r);
    if (!r.needsForce) markUndone(*st, seq);
    r.ok = !r.needsForce;
    return r;
}

Report restoreTo(Store s, const std::string& sessionId, int turnSeq, bool force) {
    Report r;
    auto st = storeAt(s);
    if (!st) {
        r.error = "snapshots are not available";
        return r;
    }
    std::lock_guard<std::mutex> lk(st->mtx);
    if (!st->open) {
        r.error = st->reason.empty() ? "snapshots are not available" : st->reason;
        return r;
    }
    std::vector<int> seqs;
    try {
        SQLite::Statement q(*st->db, "SELECT seq FROM turns WHERE session_id=? AND seq>=? AND "
                                     "ended_at IS NOT NULL AND undone=0 ORDER BY seq DESC");
        q.bind(1, sessionId.empty() ? std::string("unknown") : sessionId);
        q.bind(2, turnSeq);
        while (q.executeStep())
            seqs.push_back(q.getColumn(0).getInt());
    } catch (const std::exception& e) {
        r.error = e.what();
        return r;
    }
    if (seqs.empty()) {
        r.error = "no turn to restore in this range";
        return r;
    }
    for (int t : seqs) {
        applyTurn(*st, t, force, r);
        if (r.needsForce) break;
        markUndone(*st, t);
    }
    r.ok = !r.needsForce;
    r.turnSeq = seqs.back();
    return r;
}

void close() {
    std::lock_guard<std::mutex> reg(g_registryMutex);
    g_handles.clear();
    g_stores.clear();
}

} // namespace snapshot
