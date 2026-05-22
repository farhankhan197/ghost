#ifndef GHOST_REWRITE_LOG_HPP
#define GHOST_REWRITE_LOG_HPP

#include <string>
#include <vector>
#include <ctime>
#include <optional>

namespace ghost {
namespace rewrite {

// Event types matching git-ai's spec (simplified for Phase 1)
enum class RewriteEventType {
    RebaseStart,
    RebaseComplete,
    RebaseAbort,
    CherryPickStart,
    CherryPickComplete,
    CherryPickAbort,
    Merge,
    MergeSquash,
    Reset,
    CommitAmend,
    Stash,
    Unknown
};

std::string eventTypeToString(RewriteEventType type);
RewriteEventType parseEventType(const std::string& s);

// JSON-serializable event structures (manual JSON, no external deps)
struct RebaseStartEvent {
    std::string original_head;
    bool is_interactive;
    std::string onto_head;
    std::string toJson() const;
    static std::optional<RebaseStartEvent> fromJson(const std::string& json);
};

struct RebaseCompleteEvent {
    std::string original_head;
    std::string new_head;
    bool is_interactive;
    std::vector<std::string> original_commits;
    std::vector<std::string> new_commits;
    std::string toJson() const;
    static std::optional<RebaseCompleteEvent> fromJson(const std::string& json);
};

struct CommitAmendEvent {
    std::string original_commit;
    std::string amended_commit_sha;
    std::string toJson() const;
    static std::optional<CommitAmendEvent> fromJson(const std::string& json);
};

struct ResetEvent {
    std::string kind; // "soft", "mixed", "hard"
    std::string new_head_sha;
    std::string old_head_sha;
    std::string toJson() const;
    static std::optional<ResetEvent> fromJson(const std::string& json);
};

// Unified event container
struct RewriteEvent {
    RewriteEventType type;
    std::string json_payload;
    time_t timestamp;
};

// Log API: append and read events from the SQLite DB
class RewriteLog {
public:
    // Append an event (stores in DB, returns event id)
    static int append(const std::string& repoRoot, RewriteEventType type, const std::string& jsonPayload);

    // Load recent events (newest first, limited to maxCount)
    static std::vector<RewriteEvent> load(const std::string& repoRoot, int maxCount = 200);

    // Trim old events (keep only maxCount newest)
    static bool trim(const std::string& repoRoot, int maxCount = 200);

    // Read from stdin (for hook scripts)
    static std::vector<std::pair<std::string, std::string>> readStdinMappings();
};

} // namespace rewrite
} // namespace ghost

#endif // GHOST_REWRITE_LOG_HPP
