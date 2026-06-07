#include "db.hpp"
#include <sqlite3.h>
#include <sstream>
#include <map>

namespace ghost {
namespace persist {

static void bindText(sqlite3_stmt* stmt, int index, const std::string& value) {
    sqlite3_bind_text(stmt, index, value.c_str(), -1, SQLITE_TRANSIENT);
}

static bool stepDone(sqlite3* db, sqlite3_stmt* stmt, std::string& lastError) {
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        lastError = sqlite3_errmsg(db);
        sqlite3_finalize(stmt);
        return false;
    }
    sqlite3_finalize(stmt);
    return true;
}

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
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        lastError_ = sqlite3_errmsg(db_);
        return -1;
    }
    bindText(stmt, 1, cp.agent);
    bindText(stmt, 2, cp.model);
    bindText(stmt, 3, cp.target_file);
    bindText(stmt, 4, cp.snapshot_path);
    sqlite3_bind_int64(stmt, 5, static_cast<sqlite3_int64>(cp.ts_start));
    if (!stepDone(db_, stmt, lastError_)) return -1;
    return static_cast<int>(sqlite3_last_insert_rowid(db_));
}

std::vector<Checkpoint> Database::loadCheckpoints(bool unprocessedOnly) {
    std::vector<Checkpoint> result;
    if (!db_) return result;

    std::string sql = "SELECT id, agent, model, target_file, snapshot_path, ts_start, processed FROM checkpoints";
    if (unprocessedOnly) sql += " WHERE processed = 0";
    sql += " ORDER BY ts_start ASC;";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        lastError_ = sqlite3_errmsg(db_);
        return result;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Checkpoint cp;
        cp.id = sqlite3_column_int(stmt, 0);
        { const char* v = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)); cp.agent = v ? v : ""; }
        { const char* v = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)); cp.model = v ? v : ""; }
        { const char* v = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3)); cp.target_file = v ? v : ""; }
        { const char* v = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4)); cp.snapshot_path = v ? v : ""; }
        cp.ts_start = static_cast<time_t>(sqlite3_column_int64(stmt, 5));
        cp.processed = sqlite3_column_int(stmt, 6) != 0;
        result.push_back(cp);
    }
    sqlite3_finalize(stmt);
    return result;
}

bool Database::markCheckpointProcessed(int id) {
    if (!db_) return false;
    const char* sql = "UPDATE checkpoints SET processed = 1 WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        lastError_ = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int(stmt, 1, id);
    return stepDone(db_, stmt, lastError_);
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
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        lastError_ = sqlite3_errmsg(db_);
        return -1;
    }
    bindText(stmt, 1, sess.session_id);
    bindText(stmt, 2, sess.agent);
    bindText(stmt, 3, sess.model);
    bindText(stmt, 4, sess.author);
    sqlite3_bind_int64(stmt, 5, static_cast<sqlite3_int64>(sess.ts_start));
    sqlite3_bind_int64(stmt, 6, static_cast<sqlite3_int64>(sess.ts_end));
    sqlite3_bind_int(stmt, 7, sess.additions);
    sqlite3_bind_int(stmt, 8, sess.deletions);
    bindText(stmt, 9, sess.json_data);
    if (!stepDone(db_, stmt, lastError_)) return -1;
    return static_cast<int>(sqlite3_last_insert_rowid(db_));
}

std::vector<Session> Database::loadSessions(bool uncommittedOnly) {
    std::vector<Session> result;
    if (!db_) return result;

    std::string sql = "SELECT id, session_id, agent, model, author, ts_start, ts_end, additions, deletions, json_data, committed FROM sessions";
    if (uncommittedOnly) sql += " WHERE committed = 0";
    sql += " ORDER BY ts_start ASC;";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        lastError_ = sqlite3_errmsg(db_);
        return result;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Session s;
        s.id = sqlite3_column_int(stmt, 0);
        { const char* v = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)); s.session_id = v ? v : ""; }
        { const char* v = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)); s.agent = v ? v : ""; }
        { const char* v = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3)); s.model = v ? v : ""; }
        { const char* v = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4)); s.author = v ? v : ""; }
        s.ts_start = static_cast<time_t>(sqlite3_column_int64(stmt, 5));
        s.ts_end = static_cast<time_t>(sqlite3_column_int64(stmt, 6));
        s.additions = sqlite3_column_int(stmt, 7);
        s.deletions = sqlite3_column_int(stmt, 8);
        { const char* v = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9)); s.json_data = v ? v : ""; }
        s.committed = sqlite3_column_int(stmt, 10) != 0;
        result.push_back(s);
    }
    sqlite3_finalize(stmt);
    return result;
}

bool Database::markSessionCommitted(int id) {
    if (!db_) return false;
    const char* sql = "UPDATE sessions SET committed = 1 WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        lastError_ = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int(stmt, 1, id);
    return stepDone(db_, stmt, lastError_);
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
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        lastError_ = sqlite3_errmsg(db_);
        return false;
    }
    bindText(stmt, 1, entry.commit_sha);
    bindText(stmt, 2, entry.note_ref);
    sqlite3_bind_int(stmt, 3, entry.note_exists ? 1 : 0);
    sqlite3_bind_int(stmt, 4, entry.session_count);
    sqlite3_bind_int64(stmt, 5, static_cast<sqlite3_int64>(entry.timestamp));
    return stepDone(db_, stmt, lastError_);
}

std::optional<NoteIndexEntry> Database::getNoteIndex(const std::string& commitSha) {
    if (!db_) return std::nullopt;
    const char* sql = "SELECT commit_sha, note_ref, note_exists, session_count, timestamp FROM note_index WHERE commit_sha = ?;";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return std::nullopt;
    bindText(stmt, 1, commitSha);

    std::optional<NoteIndexEntry> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        NoteIndexEntry e;
        { const char* v = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)); e.commit_sha = v ? v : ""; }
        { const char* v = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)); e.note_ref = v ? v : ""; }
        e.note_exists = sqlite3_column_int(stmt, 2) != 0;
        e.session_count = sqlite3_column_int(stmt, 3);
        e.timestamp = static_cast<time_t>(sqlite3_column_int64(stmt, 4));
        result = e;
    }
    sqlite3_finalize(stmt);
    return result;
}

std::vector<NoteIndexEntry> Database::getAllNoteIndex() {
    std::vector<NoteIndexEntry> result;
    if (!db_) return result;

    const char* sql = "SELECT commit_sha, note_ref, note_exists, session_count, timestamp FROM note_index ORDER BY timestamp DESC;";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        lastError_ = sqlite3_errmsg(db_);
        return result;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        NoteIndexEntry e;
        { const char* v = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)); e.commit_sha = v ? v : ""; }
        { const char* v = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)); e.note_ref = v ? v : ""; }
        e.note_exists = sqlite3_column_int(stmt, 2) != 0;
        e.session_count = sqlite3_column_int(stmt, 3);
        e.timestamp = static_cast<time_t>(sqlite3_column_int64(stmt, 4));
        result.push_back(e);
    }
    sqlite3_finalize(stmt);
    return result;
}

bool Database::deleteNoteIndex(const std::string& commitSha) {
    if (!db_) return false;
    const char* sql = "DELETE FROM note_index WHERE commit_sha = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        lastError_ = sqlite3_errmsg(db_);
        return false;
    }
    bindText(stmt, 1, commitSha);
    return stepDone(db_, stmt, lastError_);
}

// --- Rewrite Log ---

int Database::appendRewriteEvent(const std::string& eventType, const std::string& jsonData) {
    if (!db_) return -1;
    const char* sql = "INSERT INTO rewrite_log (event_type, json_data, timestamp) VALUES (?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        lastError_ = sqlite3_errmsg(db_);
        return -1;
    }
    bindText(stmt, 1, eventType);
    bindText(stmt, 2, jsonData);
    sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(std::time(nullptr)));
    if (!stepDone(db_, stmt, lastError_)) return -1;
    return static_cast<int>(sqlite3_last_insert_rowid(db_));
}

std::vector<RewriteEvent> Database::loadRewriteEvents(int limit) {
    std::vector<RewriteEvent> result;
    if (!db_) return result;

    std::ostringstream oss;
    oss << "SELECT id, event_type, json_data, timestamp FROM rewrite_log ORDER BY timestamp DESC LIMIT " << limit << ";";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, oss.str().c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        lastError_ = sqlite3_errmsg(db_);
        return result;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        RewriteEvent ev;
        ev.id = sqlite3_column_int(stmt, 0);
        { const char* v = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)); ev.event_type = v ? v : ""; }
        { const char* v = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)); ev.json_data = v ? v : ""; }
        ev.timestamp = static_cast<time_t>(sqlite3_column_int64(stmt, 3));
        result.push_back(ev);
    }
    sqlite3_finalize(stmt);
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
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        lastError_ = sqlite3_errmsg(db_);
        return false;
    }
    bindText(stmt, 1, key);
    bindText(stmt, 2, jsonData);
    sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(std::time(nullptr)));
    return stepDone(db_, stmt, lastError_);
}

std::optional<std::string> Database::loadWorkingState(const std::string& key) {
    if (!db_) return std::nullopt;
    const char* sql = "SELECT json_data FROM working_state WHERE key = ?;";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return std::nullopt;
    bindText(stmt, 1, key);

    std::optional<std::string> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* v = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        result = v ? std::optional<std::string>(v) : std::nullopt;
    }
    sqlite3_finalize(stmt);
    return result;
}

bool Database::deleteWorkingState(const std::string& key) {
    if (!db_) return false;
    const char* sql = "DELETE FROM working_state WHERE key = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        lastError_ = sqlite3_errmsg(db_);
        return false;
    }
    bindText(stmt, 1, key);
    return stepDone(db_, stmt, lastError_);
}

bool Database::clearAllWorkingState() {
    return execute("DELETE FROM working_state;");
}

// --- Recovery ---

bool Database::saveRecoverySession(const std::string& sessionId, const std::string& jsonData) {
    if (!db_) return false;
    const char* sql = "INSERT OR REPLACE INTO recovery_sessions (session_id, json_data, timestamp) VALUES (?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        lastError_ = sqlite3_errmsg(db_);
        return false;
    }
    bindText(stmt, 1, sessionId);
    bindText(stmt, 2, jsonData);
    sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(std::time(nullptr)));
    return stepDone(db_, stmt, lastError_);
}

std::vector<std::pair<std::string, std::string>> Database::loadRecoverySessions() {
    std::vector<std::pair<std::string, std::string>> result;
    if (!db_) return result;

    const char* sql = "SELECT session_id, json_data FROM recovery_sessions ORDER BY timestamp ASC;";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        lastError_ = sqlite3_errmsg(db_);
        return result;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::string sid, data;
        { const char* v = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)); sid = v ? v : ""; }
        { const char* v = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)); data = v ? v : ""; }
        result.emplace_back(sid, data);
    }
    sqlite3_finalize(stmt);
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
