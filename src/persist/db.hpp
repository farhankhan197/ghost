#ifndef GHOST_PERSIST_DB_HPP
#define GHOST_PERSIST_DB_HPP

#include <string>
#include <vector>
#include <memory>
#include <ctime>
#include <optional>

struct sqlite3;

namespace ghost {
namespace persist {

struct Checkpoint {
    int id;
    std::string agent;
    std::string model;
    std::string target_file;
    std::string snapshot_path;
    time_t ts_start;
    bool processed;
};

struct Session {
    int id;
    std::string session_id;
    std::string agent;
    std::string model;
    std::string author;
    time_t ts_start;
    time_t ts_end;
    int additions;
    int deletions;
    std::string json_data;
    bool committed;
};

struct NoteIndexEntry {
    std::string commit_sha;
    std::string note_ref;
    bool note_exists;
    int session_count;
    time_t timestamp;
};

struct RewriteEvent {
    int id;
    std::string event_type;
    std::string json_data;
    time_t timestamp;
};

class Database {
public:
    explicit Database(const std::string& dbPath);
    ~Database();

    // Disable copy, allow move
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&& other) noexcept;
    Database& operator=(Database&& other) noexcept;

    bool isOpen() const;
    std::string lastError() const;

    // --- Checkpoints ---
    int saveCheckpoint(const Checkpoint& cp);
    std::vector<Checkpoint> loadCheckpoints(bool unprocessedOnly = true);
    bool markCheckpointProcessed(int id);
    bool clearCheckpoints();

    // --- Sessions ---
    int saveSession(const Session& sess);
    std::vector<Session> loadSessions(bool uncommittedOnly = true);
    bool markSessionCommitted(int id);
    bool clearSessions();

    // --- Note Index ---
    bool updateNoteIndex(const NoteIndexEntry& entry);
    std::optional<NoteIndexEntry> getNoteIndex(const std::string& commitSha);
    std::vector<NoteIndexEntry> getAllNoteIndex();
    bool deleteNoteIndex(const std::string& commitSha);

    // --- Rewrite Log ---
    int appendRewriteEvent(const std::string& eventType, const std::string& jsonData);
    std::vector<RewriteEvent> loadRewriteEvents(int limit = 200);
    bool trimRewriteEvents(int maxCount);

    // --- Working State ---
    bool saveWorkingState(const std::string& key, const std::string& jsonData);
    std::optional<std::string> loadWorkingState(const std::string& key);
    bool deleteWorkingState(const std::string& key);
    bool clearAllWorkingState();

    // --- Recovery ---
    bool saveRecoverySession(const std::string& sessionId, const std::string& jsonData);
    std::vector<std::pair<std::string, std::string>> loadRecoverySessions();
    bool clearRecoverySessions();

private:
    sqlite3* db_;
    std::string lastError_;

    bool initSchema();
    bool execute(const std::string& sql);
};

// Global singleton accessor (lazy-init per repo)
Database* getRepoDb(const std::string& repoRoot);
void closeRepoDb(const std::string& repoRoot);

} // namespace persist
} // namespace ghost

#endif // GHOST_PERSIST_DB_HPP
