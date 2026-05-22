#ifndef GHOST_COMMIT_NOTE_INDEX_HPP
#define GHOST_COMMIT_NOTE_INDEX_HPP

#include <string>
#include <vector>
#include <ctime>
#include <optional>

namespace ghost {
namespace commit {

struct NoteIndexEntry {
    std::string commit_sha;
    std::string note_ref;
    bool note_exists;
    int session_count;
    time_t timestamp;
};

// Lightweight index: which commits have ghost notes
class NoteIndex {
public:
    static bool update(const std::string& repoRoot,
                       const std::string& commitSha,
                       const std::string& noteRef,
                       bool noteExists,
                       int sessionCount);

    static std::optional<NoteIndexEntry> get(const std::string& repoRoot,
                                             const std::string& commitSha);

    static std::vector<NoteIndexEntry> getAll(const std::string& repoRoot);

    static bool remove(const std::string& repoRoot,
                       const std::string& commitSha);

    // Bulk update after processing rewrite events
    static bool migrateEntry(const std::string& repoRoot,
                             const std::string& oldSha,
                             const std::string& newSha);
};

} // namespace commit
} // namespace ghost

#endif // GHOST_COMMIT_NOTE_INDEX_HPP
