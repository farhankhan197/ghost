#include "note_index.hpp"
#include "persist/db.hpp"

namespace ghost {
namespace commit {

bool NoteIndex::update(const std::string& repoRoot,
                       const std::string& commitSha,
                       const std::string& noteRef,
                       bool noteExists,
                       int sessionCount) {
    auto* db = persist::getRepoDb(repoRoot);
    if (!db) return false;

    persist::NoteIndexEntry entry;
    entry.commit_sha = commitSha;
    entry.note_ref = noteRef;
    entry.note_exists = noteExists;
    entry.session_count = sessionCount;
    entry.timestamp = std::time(nullptr);
    return db->updateNoteIndex(entry);
}

std::optional<NoteIndexEntry> NoteIndex::get(const std::string& repoRoot,
                                             const std::string& commitSha) {
    auto* db = persist::getRepoDb(repoRoot);
    if (!db) return std::nullopt;

    auto e = db->getNoteIndex(commitSha);
    if (!e.has_value()) return std::nullopt;

    NoteIndexEntry result;
    result.commit_sha = e->commit_sha;
    result.note_ref = e->note_ref;
    result.note_exists = e->note_exists;
    result.session_count = e->session_count;
    result.timestamp = e->timestamp;
    return result;
}

std::vector<NoteIndexEntry> NoteIndex::getAll(const std::string& repoRoot) {
    auto* db = persist::getRepoDb(repoRoot);
    if (!db) return {};

    std::vector<NoteIndexEntry> result;
    for (const auto& e : db->getAllNoteIndex()) {
        NoteIndexEntry entry;
        entry.commit_sha = e.commit_sha;
        entry.note_ref = e.note_ref;
        entry.note_exists = e.note_exists;
        entry.session_count = e.session_count;
        entry.timestamp = e.timestamp;
        result.push_back(entry);
    }
    return result;
}

bool NoteIndex::remove(const std::string& repoRoot,
                       const std::string& commitSha) {
    auto* db = persist::getRepoDb(repoRoot);
    if (!db) return false;
    return db->deleteNoteIndex(commitSha);
}

bool NoteIndex::migrateEntry(const std::string& repoRoot,
                             const std::string& oldSha,
                             const std::string& newSha) {
    auto* db = persist::getRepoDb(repoRoot);
    if (!db) return false;

    auto oldEntry = db->getNoteIndex(oldSha);
    if (!oldEntry.has_value()) return true; // nothing to migrate

    // Copy to new SHA
    persist::NoteIndexEntry newEntry = oldEntry.value();
    newEntry.commit_sha = newSha;
    newEntry.timestamp = std::time(nullptr);
    bool ok = db->updateNoteIndex(newEntry);

    // Optionally delete old entry (old SHA is unreachable after rebase)
    // For safety, keep it — old SHAs may still be referenced by reflog
    (void)oldSha;
    return ok;
}

} // namespace commit
} // namespace ghost
