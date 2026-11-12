#include "snapshot.h"

#include "config.h"
#include "debug_log.h"
#include "tools/sha256.h"

#include <SQLiteCpp/SQLiteCpp.h>

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

namespace snapshot {
namespace {

// One file over this is not snapshotted: a source tree's business is in text,
// and a 200 MB asset would be copied on every edit.
constexpr long long kMaxFileBytes = 256 * 1024;

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

struct State {
    bool open = false;
    std::string reason;
    fs::path projvDir;
    fs::path workspace;
    fs::path root;    // <projv>/snapshots/<hash>
    fs::path blobDir; // <root>/blobs
    std::string dbPath;
    std::unique_ptr<SQLite::Database> db;
    std::map<std::string, Meta> begin; // the workspace as this turn started
    int currentSeq = -1;
};

State g;
std::mutex g_mutex;

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
void scan(std::map<std::string, Meta>& out) {
    out.clear();
    std::error_code ec;
    fs::recursive_directory_iterator it(g.workspace, fs::directory_options::skip_permission_denied,
                                        ec);
    if (ec) return;
    const fs::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        const fs::directory_entry& e = *it;
        if (under(e.path(), g.projvDir)) {
            it.disable_recursion_pending();
            continue;
        }
        if (e.is_directory(ec)) {
            if (isSkippedDir(e.path().filename().string())) it.disable_recursion_pending();
            continue;
        }
        if (!e.is_regular_file(ec)) continue;
        const std::string rel = fs::relative(e.path(), g.workspace, ec).generic_string();
        if (ec || rel.empty()) continue;
        Meta m;
        m.mtime = stamp(e.last_write_time(ec));
        if (ec) continue;
        m.size = (std::int64_t)e.file_size(ec);
        if (ec) continue;
        out[rel] = m;
    }
}

fs::path blobPath(const std::string& hash) {
    return g.blobDir / hash.substr(0, 2) / hash;
}

bool haveBlob(const std::string& hash) {
    if (hash.empty()) return false;
    std::error_code ec;
    return fs::exists(blobPath(hash), ec) && !ec;
}

// Write through a temporary file: a crash mid-write must not leave a truncated
// blob behind a valid hash.
bool storeBlob(const std::string& content, const std::string& hash) {
    if (haveBlob(hash)) return true;
    std::error_code ec;
    const fs::path target = blobPath(hash);
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

bool readBlob(const std::string& hash, std::string& out) {
    if (hash.empty()) return false;
    return readFile(blobPath(hash), out);
}

} // namespace

namespace {

bool schema() {
    try {
        g.db->exec("CREATE TABLE IF NOT EXISTS turns("
                   "seq INTEGER PRIMARY KEY AUTOINCREMENT,"
                   "started_at TEXT NOT NULL,"
                   "ended_at TEXT,"
                   "session_id TEXT NOT NULL,"
                   "undone INTEGER NOT NULL DEFAULT 0);");
        g.db->exec("CREATE TABLE IF NOT EXISTS changes("
                   "turn_seq INTEGER NOT NULL,"
                   "path TEXT NOT NULL,"
                   "op TEXT NOT NULL,"
                   "old_hash TEXT,"
                   "new_hash TEXT,"
                   "PRIMARY KEY(turn_seq, path));");
        g.db->exec("CREATE TABLE IF NOT EXISTS files("
                   "path TEXT PRIMARY KEY,"
                   "mtime INTEGER NOT NULL,"
                   "size INTEGER NOT NULL,"
                   "hash TEXT NOT NULL);");
        g.db->exec("CREATE INDEX IF NOT EXISTS idx_turns_session ON turns(session_id, seq);");
        return true;
    } catch (const std::exception& e) {
        debugLogErrf("[snapshot] schema failed: %s", e.what());
        return false;
    }
}

unsigned long long workspaceBytes() {
    std::map<std::string, Meta> st;
    scan(st);
    unsigned long long total = 0;
    for (const auto& [rel, m] : st)
        if (m.size > 0) total += (unsigned long long)m.size;
    return total;
}

// Remove stores nobody touched for keepDays. Only directories that contain an
// index.db are considered ours: anything else in snapshots/ is left alone.
void pruneStale(int keepDays, const fs::path& snapshotsRoot) {
    if (keepDays <= 0) return;
    std::error_code ec;
    const auto cutoff = fs::file_time_type::clock::now() - std::chrono::hours(24 * keepDays);
    for (const auto& e : fs::directory_iterator(snapshotsRoot, ec)) {
        if (ec) break;
        if (!e.is_directory(ec)) continue;
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
bool takeBaseline(std::string& err) {
    try {
        SQLite::Statement count(*g.db, "SELECT COUNT(*) FROM files");
        if (count.executeStep() && count.getColumn(0).getInt() > 0) return true;
    } catch (const std::exception&) {
    }

    std::map<std::string, Meta> st;
    scan(st);
    size_t stored = 0;
    try {
        SQLite::Transaction tr(*g.db);
        SQLite::Statement ins(*g.db,
                              "INSERT OR REPLACE INTO files(path,mtime,size,hash) VALUES(?,?,?,?)");
        for (const auto& [rel, m] : st) {
            if (m.size < 0 || m.size > kMaxFileBytes) continue;
            std::string content;
            if (!readFile(g.workspace / rel, content)) continue;
            if ((long long)content.size() > kMaxFileBytes || looksBinary(content)) continue;
            const std::string hash = sha256::hexOf(content);
            if (!storeBlob(content, hash)) continue;
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
    debugLogInfof("[snapshot] baseline: %zu file(s), %zu tracked", st.size(), stored);
    return true;
}

std::string hashOfFile(const std::string& rel, bool& ok, bool& torn) {
    ok = false;
    torn = false;
    const fs::path p = g.workspace / rel;
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
    if (!storeBlob(content, hash)) return std::string();
    ok = true;
    return hash;
}

} // namespace

bool open(const std::string& workspace, int keepDays, int maxWorkspaceGb, std::string& err) {
    std::lock_guard<std::mutex> lk(g_mutex);
    err.clear();
    if (g.open) return true;
    if (!g.reason.empty()) {
        err = g.reason;
        return false;
    }
    if (workspace.empty()) {
        g.reason = "no workspace";
        err = g.reason;
        return false;
    }

    std::error_code ec;
    g.workspace = fs::weakly_canonical(workspace, ec);
    if (ec) g.workspace = fs::absolute(workspace, ec);
    if (ec) {
        g.reason = "cannot resolve the workspace path";
        err = g.reason;
        return false;
    }
    g.projvDir = getProjvDir();

    const std::string wsHash = sha256::hexOf(lower(g.workspace.string()));
    g.root = fs::path(g.projvDir) / "snapshots" / wsHash.substr(0, 16);
    g.blobDir = g.root / "blobs";
    fs::create_directories(g.blobDir, ec);
    if (ec) {
        g.reason = "cannot create " + g.blobDir.string();
        debugLogErrf("[snapshot] %s", g.reason.c_str());
        err = g.reason;
        return false;
    }

    if (maxWorkspaceGb > 0) {
        const unsigned long long limit =
            (unsigned long long)maxWorkspaceGb * 1024ull * 1024ull * 1024ull;
        const unsigned long long bytes = workspaceBytes();
        if (bytes > limit) {
            char msg[256];
            std::snprintf(msg, sizeof(msg),
                          "workspace is %.1f GB, over the %d GB snapshot limit; "
                          "undo is disabled (raise snapshots_max_workspace_gb in config.toml)",
                          (double)bytes / (1024.0 * 1024.0 * 1024.0), maxWorkspaceGb);
            g.reason = msg;
            err = g.reason;
            debugLogWarnf("[snapshot] %s", g.reason.c_str());
            return false;
        }
    }

    pruneStale(keepDays, fs::path(g.projvDir) / "snapshots");

    g.dbPath = (g.root / "index.db").string();
    try {
        g.db = std::make_unique<SQLite::Database>(g.dbPath,
                                                  SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        g.db->exec("PRAGMA journal_mode=WAL;");
        g.db->exec("PRAGMA synchronous=NORMAL;");
    } catch (const std::exception& e) {
        g.reason = std::string("cannot open ") + g.dbPath + ": " + e.what();
        debugLogErrf("[snapshot] %s", g.reason.c_str());
        err = g.reason;
        g.db.reset();
        return false;
    }
    if (!schema()) {
        g.reason = "cannot create the snapshot tables";
        err = g.reason;
        debugLogErrf("[snapshot] %s (%s)", g.reason.c_str(), g.dbPath.c_str());
        return false;
    }
    if (!takeBaseline(err)) {
        g.reason = err;
        return false;
    }
    g.open = true;
    debugLogInfof("[snapshot] open %s (session store %s)", g.workspace.string().c_str(),
                  g.root.string().c_str());
    return true;
}

namespace {

bool setEnded(int seq) {
    try {
        SQLite::Statement up(*g.db, "UPDATE turns SET ended_at=? WHERE seq=?");
        up.bind(1, nowIso());
        up.bind(2, seq);
        up.exec();
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void dropTurn(int seq) {
    try {
        SQLite::Statement del(*g.db, "DELETE FROM turns WHERE seq=?");
        del.bind(1, seq);
        del.exec();
    } catch (const std::exception&) {
    }
}

std::string storedHash(const std::string& rel) {
    try {
        SQLite::Statement q(*g.db, "SELECT hash FROM files WHERE path=?");
        q.bind(1, rel);
        if (q.executeStep()) return q.getColumn(0).getString();
    } catch (const std::exception&) {
    }
    return std::string();
}

void upsertFile(const std::string& rel, const Meta& m, const std::string& hash) {
    try {
        SQLite::Statement up(*g.db,
                             "INSERT OR REPLACE INTO files(path,mtime,size,hash) VALUES(?,?,?,?)");
        up.bind(1, rel);
        up.bind(2, m.mtime);
        up.bind(3, m.size);
        up.bind(4, hash);
        up.exec();
    } catch (const std::exception&) {
    }
}

void forgetFile(const std::string& rel) {
    try {
        SQLite::Statement del(*g.db, "DELETE FROM files WHERE path=?");
        del.bind(1, rel);
        del.exec();
    } catch (const std::exception&) {
    }
}

void recordChange(int seq, const std::string& rel, const char* op, const std::string& oldHash,
                  const std::string& newHash) {
    try {
        SQLite::Statement ins(*g.db,
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
std::string currentHash(const std::string& rel) {
    std::string content;
    if (!readFile(g.workspace / rel, content)) return std::string();
    if ((long long)content.size() > kMaxFileBytes || looksBinary(content)) return std::string();
    return sha256::hexOf(content);
}

bool writeRestored(const std::string& rel, const std::string& content) {
    const fs::path target = g.workspace / rel;
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

std::vector<Change> changesOf(int seq) {
    std::vector<Change> out;
    try {
        SQLite::Statement q(*g.db, "SELECT path,op,old_hash,new_hash FROM changes WHERE turn_seq=? "
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

// Undo one recorded turn. `force` skips the "file changed after the turn"
// check the first time round, so the caller can ask the user.
void applyTurn(int seq, bool force, Report& r) {
    const std::vector<Change> changes = changesOf(seq);

    // Check everything before writing anything. A half-applied turn that then
    // reports "not executed" is worse than no undo at all: the message would be
    // a lie, and the user believes the workspace was left alone.
    if (!force) {
        for (const Change& c : changes) {
            bool untouched = false;
            if (c.op == "A") {
                const std::string now = currentHash(c.path);
                untouched = now.empty() || now == c.newHash;
            } else {
                const std::string now = currentHash(c.path);
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
            if (fs::remove(g.workspace / c.path, ec) && !ec) {
                r.removed.push_back(c.path);
                forgetFile(c.path);
            }
            continue;
        }

        // M and D both restore the old content.
        if (c.oldHash.empty() || !haveBlob(c.oldHash)) {
            r.skipped.push_back(c.path);
            continue;
        }
        std::string content;
        if (!readBlob(c.oldHash, content)) {
            r.skipped.push_back(c.path);
            continue;
        }
        if (writeRestored(c.path, content)) {
            r.restored.push_back(c.path);
            std::error_code ec;
            Meta m;
            m.size = (std::int64_t)content.size();
            m.mtime = stamp(fs::last_write_time(g.workspace / c.path, ec));
            if (!ec) upsertFile(c.path, m, c.oldHash);
        } else {
            r.skipped.push_back(c.path);
        }
    }
    r.turnSeq = seq;
}

void markUndone(int seq) {
    try {
        SQLite::Statement up(*g.db, "UPDATE turns SET undone=1 WHERE seq=?");
        up.bind(1, seq);
        up.exec();
    } catch (const std::exception&) {
    }
}

} // namespace

bool isAvailable() {
    return g.open;
}

std::string unavailableReason() {
    return g.reason;
}

int beginTurn(const std::string& sessionId, std::string& err) {
    std::lock_guard<std::mutex> lk(g_mutex);
    err.clear();
    g.currentSeq = -1;
    if (!g.open) {
        err = g.reason.empty() ? "snapshots are not available" : g.reason;
        return -1;
    }
    scan(g.begin);
    try {
        SQLite::Statement ins(*g.db, "INSERT INTO turns(started_at,session_id) VALUES(?,?)");
        ins.bind(1, nowIso());
        ins.bind(2, sessionId.empty() ? std::string("unknown") : sessionId);
        ins.exec();
        g.currentSeq = (int)g.db->getLastInsertRowid();
    } catch (const std::exception& e) {
        err = std::string("cannot start a snapshot turn: ") + e.what();
        return -1;
    }
    return g.currentSeq;
}

bool endTurn(int turnSeq, std::string& err) {
    std::lock_guard<std::mutex> lk(g_mutex);
    err.clear();
    if (!g.open || turnSeq < 0) return false;

    std::map<std::string, Meta> after;
    scan(after);
    if (after == g.begin) {
        // Nothing moved: do not leave an empty turn behind, or /undo would
        // spend a click on it.
        dropTurn(turnSeq);
        g.begin.clear();
        return true;
    }

    size_t recorded = 0;
    for (const auto& [rel, m] : after) {
        auto it = g.begin.find(rel);
        const bool added = (it == g.begin.end());
        if (!added && it->second.mtime == m.mtime && it->second.size == m.size) continue;

        bool ok = false, torn = false;
        const std::string hash = hashOfFile(rel, ok, torn);
        if (!ok) continue; // binary, too large, unreadable or being written
        const std::string oldHash = added ? std::string() : storedHash(rel);
        recordChange(turnSeq, rel, (added && oldHash.empty()) ? "A" : "M", oldHash, hash);
        upsertFile(rel, m, hash);
        ++recorded;
    }
    for (const auto& [rel, m] : g.begin) {
        if (after.count(rel)) continue;
        const std::string oldHash = storedHash(rel);
        if (oldHash.empty()) continue;
        recordChange(turnSeq, rel, "D", oldHash, std::string());
        forgetFile(rel);
        ++recorded;
    }
    g.begin.clear();
    if (recorded == 0) {
        dropTurn(turnSeq);
        return true;
    }
    if (!setEnded(turnSeq)) {
        err = "cannot close the snapshot turn";
        return false;
    }
    debugLogInfof("[snapshot] turn %d: %zu change(s)", turnSeq, recorded);
    return true;
}

int lastTurn(const std::string& sessionId) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g.open) return -1;
    try {
        SQLite::Statement q(
            *g.db, "SELECT MAX(seq) FROM turns WHERE session_id=? AND ended_at IS NOT NULL "
                   "AND undone=0");
        q.bind(1, sessionId.empty() ? std::string("unknown") : sessionId);
        if (q.executeStep() && !q.getColumn(0).isNull()) return q.getColumn(0).getInt();
    } catch (const std::exception&) {
    }
    return -1;
}

Report undoLast(const std::string& sessionId, bool force) {
    std::lock_guard<std::mutex> lk(g_mutex);
    Report r;
    if (!g.open) {
        r.error = g.reason.empty() ? "snapshots are not available" : g.reason;
        return r;
    }
    int seq = -1;
    try {
        SQLite::Statement q(
            *g.db, "SELECT MAX(seq) FROM turns WHERE session_id=? AND ended_at IS NOT NULL "
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
    applyTurn(seq, force, r);
    if (!r.needsForce) markUndone(seq);
    r.ok = !r.needsForce;
    return r;
}

Report restoreTo(const std::string& sessionId, int turnSeq, bool force) {
    std::lock_guard<std::mutex> lk(g_mutex);
    Report r;
    if (!g.open) {
        r.error = g.reason.empty() ? "snapshots are not available" : g.reason;
        return r;
    }
    std::vector<int> seqs;
    try {
        SQLite::Statement q(*g.db, "SELECT seq FROM turns WHERE session_id=? AND seq>=? AND "
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
    for (int s : seqs) {
        applyTurn(s, force, r);
        if (r.needsForce) break;
        markUndone(s);
    }
    r.ok = !r.needsForce;
    r.turnSeq = seqs.back();
    return r;
}

void close() {
    std::lock_guard<std::mutex> lk(g_mutex);
    g.db.reset();
    g.open = false;
    g.begin.clear();
    g.currentSeq = -1;
    g.reason.clear();
    g.root.clear();
    g.blobDir.clear();
    g.dbPath.clear();
    g.workspace.clear();
    g.projvDir.clear();
}

} // namespace snapshot
