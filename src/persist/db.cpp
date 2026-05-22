#include "db.hpp"
#include "sqlite3.h"
#include <cstring>
#include <sstream>
#include <map>

namespace ghost {
namespace persist {

// --- Helper: escape single quotes in strings ---
static std::string sqlEscape(const std::string& s) {
    std::string result;
    result.reserve(s.size() * 2);
    for (char c : s) {
        if (c == '\'') result += "''";
        else result += c;
    }
    return result;
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
    std::ostringstream oss;
    oss << "INSERT INTO checkpoints (agent, model, target_file, snapshot_path, ts_start, processed) VALUES ('"
        << sqlEscape(cp.agent) << "', '"
        << sqlEscape(cp.model) << "', '"
        << sqlEscape(cp.target_file) << "', '"
        << sqlEscape(cp.snapshot_path) << "', "
        << static_cast<long long>(cp.ts_start) << ", 0);";
    if (!execute(oss.str())) return -1;
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
        cp.agent = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        cp.model = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        cp.target_file = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        cp.snapshot_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        cp.ts_start = static_cast<time_t>(sqlite3_column_int64(stmt, 5));
        cp.processed = sqlite3_column_int(stmt, 6) != 0;
        result.push_back(cp);
    }
    sqlite3_finalize(stmt);
    return result;
}

bool Database::markCheckpointProcessed(int id) {
    if (!db_) return false;
    std::ostringstream oss;
    oss << "UPDATE checkpoints SET processed = 1 WHERE id = " << id << ";";
    return execute(oss.str());
}

bool Database::clearCheckpoints() {
    return execute("DELETE FROM checkpoints;");
}

// --- Sessions ---

int Database::saveSession(const Session& sess) {
    if (!db_) return -1;
    std::ostringstream oss;
    oss << "INSERT OR REPLACE INTO sessions (session_id, agent, model, author, ts_start, ts_end, additions, deletions, json_data, committed) VALUES ('"
        << sqlEscape(sess.session_id) << "', '"
        << sqlEscape(sess.agent) << "', '"
        << sqlEscape(sess.model) << "', '"
        << sqlEscape(sess.author) << "', "
        << static_cast<long long>(sess.ts_start) << ", "
        << static_cast<long long>(sess.ts_end) << ", "
        << sess.additions << ", "
        << sess.deletions << ", '"
        << sqlEscape(sess.json_data) << "', 0);";
    if (!execute(oss.str())) return -1;
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
        s.session_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        s.agent = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        s.model = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        s.author = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        s.ts_start = static_cast<time_t>(sqlite3_column_int64(stmt, 5));
        s.ts_end = static_cast<time_t>(sqlite3_column_int64(stmt, 6));
        s.additions = sqlite3_column_int(stmt, 7);
        s.deletions = sqlite3_column_int(stmt, 8);
        s.json_data = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
        s.committed = sqlite3_column_int(stmt, 10) != 0;
        result.push_back(s);
    }
    sqlite3_finalize(stmt);
    return result;
}

bool Database::markSessionCommitted(int id) {
    if (!db_) return false;
    std::ostringstream oss;
    oss << "UPDATE sessions SET committed = 1 WHERE id = " << id << ";";
    return execute(oss.str());
}

bool Database::clearSessions() {
    return execute("DELETE FROM sessions WHERE committed = 1;");
}

// --- Note Index ---

bool Database::updateNoteIndex(const NoteIndexEntry& entry) {
    if (!db_) return false;
    std::ostringstream oss;
    oss << "INSERT OR REPLACE INTO note_index (commit_sha, note_ref, note_exists, session_count, timestamp) VALUES ('"
        << sqlEscape(entry.commit_sha) << "', '"
        << sqlEscape(entry.note_ref) << "', "
        << (entry.note_exists ? 1 : 0) << ", "
        << entry.session_count << ", "
        << static_cast<long long>(entry.timestamp) << ");";
    return execute(oss.str());
}

std::optional<NoteIndexEntry> Database::getNoteIndex(const std::string& commitSha) {
    if (!db_) return std::nullopt;
    std::ostringstream oss;
    oss << "SELECT commit_sha, note_ref, note_exists, session_count, timestamp FROM note_index WHERE commit_sha = '"
        << sqlEscape(commitSha) << "';";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, oss.str().c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return std::nullopt;

    std::optional<NoteIndexEntry> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        NoteIndexEntry e;
        e.commit_sha = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        e.note_ref = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
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
        e.commit_sha = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        e.note_ref = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
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
    std::ostringstream oss;
    oss << "DELETE FROM note_index WHERE commit_sha = '" << sqlEscape(commitSha) << "';";
    return execute(oss.str());
}

// --- Rewrite Log ---

int Database::appendRewriteEvent(const std::string& eventType, const std::string& jsonData) {
    if (!db_) return -1;
    std::ostringstream oss;
    oss << "INSERT INTO rewrite_log (event_type, json_data, timestamp) VALUES ('"
        << sqlEscape(eventType) << "', '"
        << sqlEscape(jsonData) << "', "
        << static_cast<long long>(std::time(nullptr)) << ");";
    if (!execute(oss.str())) return -1;
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
        ev.event_type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        ev.json_data = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
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
    std::ostringstream oss;
    oss << "INSERT OR REPLACE INTO working_state (key, json_data, timestamp) VALUES ('"
        << sqlEscape(key) << "', '"
        << sqlEscape(jsonData) << "', "
        << static_cast<long long>(std::time(nullptr)) << ");";
    return execute(oss.str());
}

std::optional<std::string> Database::loadWorkingState(const std::string& key) {
    if (!db_) return std::nullopt;
    std::ostringstream oss;
    oss << "SELECT json_data FROM working_state WHERE key = '" << sqlEscape(key) << "';";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, oss.str().c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return std::nullopt;

    std::optional<std::string> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        result = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return result;
}

bool Database::deleteWorkingState(const std::string& key) {
    if (!db_) return false;
    std::ostringstream oss;
    oss << "DELETE FROM working_state WHERE key = '" << sqlEscape(key) << "';";
    return execute(oss.str());
}

bool Database::clearAllWorkingState() {
    return execute("DELETE FROM working_state;");
}

// --- Recovery ---

bool Database::saveRecoverySession(const std::string& sessionId, const std::string& jsonData) {
    if (!db_) return false;
    std::ostringstream oss;
    oss << "INSERT OR REPLACE INTO recovery_sessions (session_id, json_data, timestamp) VALUES ('"
        << sqlEscape(sessionId) << "', '"
        << sqlEscape(jsonData) << "', "
        << static_cast<long long>(std::time(nullptr)) << ");";
    return execute(oss.str());
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
        std::string sid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        std::string data = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
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
