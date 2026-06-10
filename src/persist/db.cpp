#include "db.hpp"
#include <sqlite3.h>
#include <sstream>
#include <map>

namespace ghost {
namespace persist {

class Statement {
public:
    Statement(sqlite3* db, const std::string& sql, std::string& lastError)
        : db_(db), stmt_(nullptr), lastError_(lastError) {
        if (!db_) return;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt_, nullptr) != SQLITE_OK) {
            lastError_ = sqlite3_errmsg(db_);
            stmt_ = nullptr;
        }
    }

    ~Statement() {
        if (stmt_) sqlite3_finalize(stmt_);
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    bool ok() const { return stmt_ != nullptr; }
    int step() { return stmt_ ? sqlite3_step(stmt_) : SQLITE_MISUSE; }

    bool done() {
        if (!stmt_) return false;
        int rc = sqlite3_step(stmt_);
        if (rc != SQLITE_DONE) {
            lastError_ = sqlite3_errmsg(db_);
            return false;
        }
        return true;
    }

    void bindText(int index, const std::string& value) {
        sqlite3_bind_text(stmt_, index, value.c_str(), -1, SQLITE_TRANSIENT);
    }

    void bindInt(int index, int value) {
        sqlite3_bind_int(stmt_, index, value);
    }

    void bindInt64(int index, sqlite3_int64 value) {
        sqlite3_bind_int64(stmt_, index, value);
    }

    std::string columnText(int index) const {
        const char* value = reinterpret_cast<const char*>(sqlite3_column_text(stmt_, index));
        return value ? value : "";
    }

    int columnInt(int index) const {
        return sqlite3_column_int(stmt_, index);
    }

    sqlite3_int64 columnInt64(int index) const {
        return sqlite3_column_int64(stmt_, index);
    }

private:
    sqlite3* db_;
    sqlite3_stmt* stmt_;
    std::string& lastError_;
};

// --- Constructor / Destructor ---

Database::Database(const std::string& dbPath) : db_(nullptr) {
    int rc = sqlite3_open(dbPath.c_str(), &db_);
    if (rc != SQLITE_OK) {
        lastError_ = sqlite3_errmsg(db_);
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        return;
    }
    initSchema();
}

Database::~Database() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

Database::Database(Database&& other) noexcept : db_(other.db_), lastError_(std::move(other.lastError_)) {
    other.db_ = nullptr;
}

Database& Database::operator=(Database&& other) noexcept {
    if (this != &other) {
        if (db_) sqlite3_close(db_);
        db_ = other.db_;
        lastError_ = std::move(other.lastError_);
        other.db_ = nullptr;
    }
    return *this;
}

bool Database::isOpen() const {
    return db_ != nullptr;
}

std::string Database::lastError() const {
    return lastError_;
}

bool Database::execute(const std::string& sql) {
    if (!db_) return false;
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        lastError_ = errMsg ? errMsg : "unknown error";
        if (errMsg) sqlite3_free(errMsg);
        return false;
    }
    return true;
}

bool Database::initSchema() {
    if (!db_) return false;

    const char* schema = R"SQL(
        PRAGMA journal_mode = WAL;
        PRAGMA synchronous = NORMAL;

        CREATE TABLE IF NOT EXISTS checkpoints (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            agent TEXT NOT NULL,
            model TEXT,
            target_file TEXT NOT NULL,
            snapshot_path TEXT NOT NULL,
            ts_start INTEGER NOT NULL,
            processed INTEGER NOT NULL DEFAULT 0
        );
        CREATE INDEX IF NOT EXISTS idx_cp_processed ON checkpoints(processed);
        CREATE INDEX IF NOT EXISTS idx_cp_file ON checkpoints(target_file);

        CREATE TABLE IF NOT EXISTS sessions (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            session_id TEXT NOT NULL UNIQUE,
            agent TEXT NOT NULL,
            model TEXT,
            author TEXT,
            ts_start INTEGER NOT NULL,
            ts_end INTEGER NOT NULL,
            additions INTEGER NOT NULL DEFAULT 0,
            deletions INTEGER NOT NULL DEFAULT 0,
            json_data TEXT NOT NULL,
            committed INTEGER NOT NULL DEFAULT 0
        );
        CREATE INDEX IF NOT EXISTS idx_sess_committed ON sessions(committed);

        CREATE TABLE IF NOT EXISTS note_index (
            commit_sha TEXT PRIMARY KEY,
            note_ref TEXT NOT NULL,
            note_exists INTEGER NOT NULL DEFAULT 0,
            session_count INTEGER NOT NULL DEFAULT 0,
            timestamp INTEGER NOT NULL
        );

        CREATE TABLE IF NOT EXISTS rewrite_log (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            event_type TEXT NOT NULL,
            json_data TEXT NOT NULL,
            timestamp INTEGER NOT NULL
        );
        CREATE INDEX IF NOT EXISTS idx_rewrite_ts ON rewrite_log(timestamp DESC);

        CREATE TABLE IF NOT EXISTS working_state (
            key TEXT PRIMARY KEY,
            json_data TEXT NOT NULL,
            timestamp INTEGER NOT NULL
        );

        CREATE TABLE IF NOT EXISTS recovery_sessions (
            session_id TEXT PRIMARY KEY,
            json_data TEXT NOT NULL,
            timestamp INTEGER NOT NULL
        );
    )SQL";

    return execute(schema);
}

// --- Checkpoints ---

int Database::saveCheckpoint(const Checkpoint& cp) {
    if (!db_) return -1;
    const char* sql =
        "INSERT INTO checkpoints (agent, model, target_file, snapshot_path, ts_start, processed) "
        "VALUES (?, ?, ?, ?, ?, 0);";
    Statement stmt(db_, sql, lastError_);
    if (!stmt.ok()) return -1;
    stmt.bindText(1, cp.agent);
    stmt.bindText(2, cp.model);
    stmt.bindText(3, cp.target_file);
    stmt.bindText(4, cp.snapshot_path);
    stmt.bindInt64(5, static_cast<sqlite3_int64>(cp.ts_start));
    if (!stmt.done()) return -1;
    return static_cast<int>(sqlite3_last_insert_rowid(db_));
}

std::vector<Checkpoint> Database::loadCheckpoints(bool unprocessedOnly) {
    std::vector<Checkpoint> result;
    if (!db_) return result;

    std::string sql = "SELECT id, agent, model, target_file, snapshot_path, ts_start, processed FROM checkpoints";
    if (unprocessedOnly) sql += " WHERE processed = 0";
    sql += " ORDER BY ts_start ASC;";

    Statement stmt(db_, sql, lastError_);
    if (!stmt.ok()) return result;

    while (stmt.step() == SQLITE_ROW) {
        Checkpoint cp;
        cp.id = stmt.columnInt(0);
        cp.agent = stmt.columnText(1);
        cp.model = stmt.columnText(2);
        cp.target_file = stmt.columnText(3);
        cp.snapshot_path = stmt.columnText(4);
        cp.ts_start = static_cast<time_t>(stmt.columnInt64(5));
        cp.processed = stmt.columnInt(6) != 0;
        result.push_back(cp);
    }
    return result;
}

bool Database::markCheckpointProcessed(int id) {
    if (!db_) return false;
    const char* sql = "UPDATE checkpoints SET processed = 1 WHERE id = ?;";
    Statement stmt(db_, sql, lastError_);
    if (!stmt.ok()) return false;
    stmt.bindInt(1, id);
    return stmt.done();
}

bool Database::clearCheckpoints() {
    return execute("DELETE FROM checkpoints;");
}

// --- Sessions ---

int Database::saveSession(const Session& sess) {
    if (!db_) return -1;
    const char* sql =
        "INSERT OR REPLACE INTO sessions "
        "(session_id, agent, model, author, ts_start, ts_end, additions, deletions, json_data, committed) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, 0);";
    Statement stmt(db_, sql, lastError_);
    if (!stmt.ok()) return -1;
    stmt.bindText(1, sess.session_id);
    stmt.bindText(2, sess.agent);
    stmt.bindText(3, sess.model);
    stmt.bindText(4, sess.author);
    stmt.bindInt64(5, static_cast<sqlite3_int64>(sess.ts_start));
    stmt.bindInt64(6, static_cast<sqlite3_int64>(sess.ts_end));
    stmt.bindInt(7, sess.additions);
    stmt.bindInt(8, sess.deletions);
    stmt.bindText(9, sess.json_data);
    if (!stmt.done()) return -1;
    return static_cast<int>(sqlite3_last_insert_rowid(db_));
}

std::vector<Session> Database::loadSessions(bool uncommittedOnly) {
    std::vector<Session> result;
    if (!db_) return result;

    std::string sql = "SELECT id, session_id, agent, model, author, ts_start, ts_end, additions, deletions, json_data, committed FROM sessions";
    if (uncommittedOnly) sql += " WHERE committed = 0";
    sql += " ORDER BY ts_start ASC;";

    Statement stmt(db_, sql, lastError_);
    if (!stmt.ok()) return result;

    while (stmt.step() == SQLITE_ROW) {
        Session s;
        s.id = stmt.columnInt(0);
        s.session_id = stmt.columnText(1);
        s.agent = stmt.columnText(2);
        s.model = stmt.columnText(3);
        s.author = stmt.columnText(4);
        s.ts_start = static_cast<time_t>(stmt.columnInt64(5));
        s.ts_end = static_cast<time_t>(stmt.columnInt64(6));
        s.additions = stmt.columnInt(7);
        s.deletions = stmt.columnInt(8);
        s.json_data = stmt.columnText(9);
        s.committed = stmt.columnInt(10) != 0;
        result.push_back(s);
    }
    return result;
}

bool Database::markSessionCommitted(int id) {
    if (!db_) return false;
    const char* sql = "UPDATE sessions SET committed = 1 WHERE id = ?;";
    Statement stmt(db_, sql, lastError_);
    if (!stmt.ok()) return false;
    stmt.bindInt(1, id);
    return stmt.done();
}

bool Database::clearSessions() {
    return execute("DELETE FROM sessions WHERE committed = 1;");
}

// --- Note Index ---

bool Database::updateNoteIndex(const NoteIndexEntry& entry) {
    if (!db_) return false;
    const char* sql =
        "INSERT OR REPLACE INTO note_index "
        "(commit_sha, note_ref, note_exists, session_count, timestamp) VALUES (?, ?, ?, ?, ?);";
    Statement stmt(db_, sql, lastError_);
    if (!stmt.ok()) return false;
    stmt.bindText(1, entry.commit_sha);
    stmt.bindText(2, entry.note_ref);
    stmt.bindInt(3, entry.note_exists ? 1 : 0);
    stmt.bindInt(4, entry.session_count);
    stmt.bindInt64(5, static_cast<sqlite3_int64>(entry.timestamp));
    return stmt.done();
}

std::optional<NoteIndexEntry> Database::getNoteIndex(const std::string& commitSha) {
    if (!db_) return std::nullopt;
    const char* sql = "SELECT commit_sha, note_ref, note_exists, session_count, timestamp FROM note_index WHERE commit_sha = ?;";
    Statement stmt(db_, sql, lastError_);
    if (!stmt.ok()) return std::nullopt;
    stmt.bindText(1, commitSha);

    std::optional<NoteIndexEntry> result;
    if (stmt.step() == SQLITE_ROW) {
        NoteIndexEntry e;
        e.commit_sha = stmt.columnText(0);
        e.note_ref = stmt.columnText(1);
        e.note_exists = stmt.columnInt(2) != 0;
        e.session_count = stmt.columnInt(3);
        e.timestamp = static_cast<time_t>(stmt.columnInt64(4));
        result = e;
    }
    return result;
}

std::vector<NoteIndexEntry> Database::getAllNoteIndex() {
    std::vector<NoteIndexEntry> result;
    if (!db_) return result;

    const char* sql = "SELECT commit_sha, note_ref, note_exists, session_count, timestamp FROM note_index ORDER BY timestamp DESC;";
    Statement stmt(db_, sql, lastError_);
    if (!stmt.ok()) return result;

    while (stmt.step() == SQLITE_ROW) {
        NoteIndexEntry e;
        e.commit_sha = stmt.columnText(0);
        e.note_ref = stmt.columnText(1);
        e.note_exists = stmt.columnInt(2) != 0;
        e.session_count = stmt.columnInt(3);
        e.timestamp = static_cast<time_t>(stmt.columnInt64(4));
        result.push_back(e);
    }
    return result;
}

bool Database::deleteNoteIndex(const std::string& commitSha) {
    if (!db_) return false;
    const char* sql = "DELETE FROM note_index WHERE commit_sha = ?;";
    Statement stmt(db_, sql, lastError_);
    if (!stmt.ok()) return false;
    stmt.bindText(1, commitSha);
    return stmt.done();
}

// --- Rewrite Log ---

int Database::appendRewriteEvent(const std::string& eventType, const std::string& jsonData) {
    if (!db_) return -1;
    const char* sql = "INSERT INTO rewrite_log (event_type, json_data, timestamp) VALUES (?, ?, ?);";
    Statement stmt(db_, sql, lastError_);
    if (!stmt.ok()) return -1;
    stmt.bindText(1, eventType);
    stmt.bindText(2, jsonData);
    stmt.bindInt64(3, static_cast<sqlite3_int64>(std::time(nullptr)));
    if (!stmt.done()) return -1;
    return static_cast<int>(sqlite3_last_insert_rowid(db_));
}

std::vector<RewriteEvent> Database::loadRewriteEvents(int limit) {
    std::vector<RewriteEvent> result;
    if (!db_) return result;

    std::ostringstream oss;
    oss << "SELECT id, event_type, json_data, timestamp FROM rewrite_log ORDER BY timestamp DESC LIMIT " << limit << ";";

    Statement stmt(db_, oss.str(), lastError_);
    if (!stmt.ok()) return result;

    while (stmt.step() == SQLITE_ROW) {
        RewriteEvent ev;
        ev.id = stmt.columnInt(0);
        ev.event_type = stmt.columnText(1);
        ev.json_data = stmt.columnText(2);
        ev.timestamp = static_cast<time_t>(stmt.columnInt64(3));
        result.push_back(ev);
    }
    return result;
}

bool Database::trimRewriteEvents(int maxCount) {
    if (!db_) return false;
    std::ostringstream oss;
    oss << "DELETE FROM rewrite_log WHERE id NOT IN (SELECT id FROM rewrite_log ORDER BY timestamp DESC LIMIT " << maxCount << ");";
    return execute(oss.str());
}

// --- Working State ---

bool Database::saveWorkingState(const std::string& key, const std::string& jsonData) {
    if (!db_) return false;
    const char* sql = "INSERT OR REPLACE INTO working_state (key, json_data, timestamp) VALUES (?, ?, ?);";
    Statement stmt(db_, sql, lastError_);
    if (!stmt.ok()) return false;
    stmt.bindText(1, key);
    stmt.bindText(2, jsonData);
    stmt.bindInt64(3, static_cast<sqlite3_int64>(std::time(nullptr)));
    return stmt.done();
}

std::optional<std::string> Database::loadWorkingState(const std::string& key) {
    if (!db_) return std::nullopt;
    const char* sql = "SELECT json_data FROM working_state WHERE key = ?;";
    Statement stmt(db_, sql, lastError_);
    if (!stmt.ok()) return std::nullopt;
    stmt.bindText(1, key);

    std::optional<std::string> result;
    if (stmt.step() == SQLITE_ROW) {
        result = stmt.columnText(0);
    }
    return result;
}

bool Database::deleteWorkingState(const std::string& key) {
    if (!db_) return false;
    const char* sql = "DELETE FROM working_state WHERE key = ?;";
    Statement stmt(db_, sql, lastError_);
    if (!stmt.ok()) return false;
    stmt.bindText(1, key);
    return stmt.done();
}

bool Database::clearAllWorkingState() {
    return execute("DELETE FROM working_state;");
}

// --- Recovery ---

bool Database::saveRecoverySession(const std::string& sessionId, const std::string& jsonData) {
    if (!db_) return false;
    const char* sql = "INSERT OR REPLACE INTO recovery_sessions (session_id, json_data, timestamp) VALUES (?, ?, ?);";
    Statement stmt(db_, sql, lastError_);
    if (!stmt.ok()) return false;
    stmt.bindText(1, sessionId);
    stmt.bindText(2, jsonData);
    stmt.bindInt64(3, static_cast<sqlite3_int64>(std::time(nullptr)));
    return stmt.done();
}

std::vector<std::pair<std::string, std::string>> Database::loadRecoverySessions() {
    std::vector<std::pair<std::string, std::string>> result;
    if (!db_) return result;

    const char* sql = "SELECT session_id, json_data FROM recovery_sessions ORDER BY timestamp ASC;";
    Statement stmt(db_, sql, lastError_);
    if (!stmt.ok()) return result;

    while (stmt.step() == SQLITE_ROW) {
        result.emplace_back(stmt.columnText(0), stmt.columnText(1));
    }
    return result;
}

bool Database::clearRecoverySessions() {
    return execute("DELETE FROM recovery_sessions;");
}

// --- Global singleton ---

static std::map<std::string, std::unique_ptr<Database>> g_dbs;

Database* getRepoDb(const std::string& repoRoot) {
    auto it = g_dbs.find(repoRoot);
    if (it != g_dbs.end() && it->second->isOpen()) {
        return it->second.get();
    }
    std::string dbPath = repoRoot + "/.git/ghost/ghost.db";
    auto db = std::make_unique<Database>(dbPath);
    if (!db->isOpen()) {
        return nullptr;
    }
    Database* ptr = db.get();
    g_dbs[repoRoot] = std::move(db);
    return ptr;
}

void closeRepoDb(const std::string& repoRoot) {
    g_dbs.erase(repoRoot);
}

} // namespace persist
} // namespace ghost
