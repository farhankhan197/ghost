#include "processor.hpp"
#include "rewrite_log.hpp"
#include "working_state.hpp"
#include "persist/db.hpp"
#include "commit/note_index.hpp"
#include <cstdio>
#include <memory>
#include <sstream>
#include <iostream>

namespace ghost {
namespace rewrite {

static std::string runGitCommand(const std::string& cmd) {
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) return "";
    std::string result;
    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
        result += buffer;
    }
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
        result.pop_back();
    }
    return result;
}

// --- Public API ---

bool Processor::processRebase(const std::string& repoRoot,
                              const std::vector<std::string>& originalCommits,
                              const std::vector<std::string>& newCommits) {
    if (originalCommits.size() != newCommits.size()) {
        std::cerr << "ghost: rebase mapping mismatch: " << originalCommits.size()
                  << " originals vs " << newCommits.size() << " new\n";
        return false;
    }

    bool anyCopied = false;
    for (size_t i = 0; i < originalCommits.size(); ++i) {
        auto idx = commit::NoteIndex::get(repoRoot, originalCommits[i]);
        if (idx.has_value() && idx->note_exists) {
            if (copyNote(repoRoot, "refs/notes/ghost", originalCommits[i], newCommits[i])) {
                anyCopied = true;
                // Update note index
                commit::NoteIndex::update(repoRoot, newCommits[i], "refs/notes/ghost", true, idx->session_count);
            }
            // Also copy verified note
            auto vidx = commit::NoteIndex::get(repoRoot, originalCommits[i]);
            if (vidx.has_value()) {
                (void)copyNote(repoRoot, "refs/notes/ghost-verified", originalCommits[i], newCommits[i]);
            }
        }
    }

    return anyCopied;
}

bool Processor::processAmend(const std::string& repoRoot,
                             const std::string& originalCommit,
                             const std::string& amendedCommit) {
    auto idx = commit::NoteIndex::get(repoRoot, originalCommit);
    if (!idx.has_value() || !idx->note_exists) {
        // No ghost note on original — nothing to migrate
        return true;
    }

    bool ok = true;
    ok = copyNote(repoRoot, "refs/notes/ghost", originalCommit, amendedCommit) && ok;
    (void)copyNote(repoRoot, "refs/notes/ghost-verified", originalCommit, amendedCommit);

    if (ok) {
        commit::NoteIndex::update(repoRoot, amendedCommit, "refs/notes/ghost", true, idx->session_count);
        commit::NoteIndex::migrateEntry(repoRoot, originalCommit, amendedCommit);
    }
    return ok;
}

bool Processor::processCherryPick(const std::string& repoRoot,
                                    const std::vector<std::string>& sourceCommits,
                                    const std::vector<std::string>& newCommits) {
    if (sourceCommits.size() != newCommits.size()) {
        std::cerr << "ghost: cherry-pick mapping mismatch\n";
        return false;
    }

    bool anyCopied = false;
    for (size_t i = 0; i < sourceCommits.size(); ++i) {
        auto idx = commit::NoteIndex::get(repoRoot, sourceCommits[i]);
        if (idx.has_value() && idx->note_exists) {
            if (copyNote(repoRoot, "refs/notes/ghost", sourceCommits[i], newCommits[i])) {
                anyCopied = true;
                commit::NoteIndex::update(repoRoot, newCommits[i], "refs/notes/ghost", true, idx->session_count);
            }
            (void)copyNote(repoRoot, "refs/notes/ghost-verified", sourceCommits[i], newCommits[i]);
        }
    }
    return anyCopied;
}

bool Processor::processMergeSquash(const std::string& repoRoot,
                                   const std::string& sourceHead,
                                   const std::string& baseHead) {
    (void)sourceHead;
    (void)baseHead;
    // For merge --squash, the changes are staged but not committed.
    // We save the working state of any existing sessions so they survive
    // until the next commit.
    WorkingState::save(repoRoot, "merge_squash");
    return true;
}

bool Processor::processReset(const std::string& repoRoot,
                             const std::string& oldHeadSha,
                             const std::string& newHeadSha,
                             const std::string& kind) {
    (void)newHeadSha;

    if (kind != "soft" && kind != "mixed") {
        // Hard reset: clear everything
        if (kind == "hard") {
            auto* db = persist::getRepoDb(repoRoot);
            if (db) {
                db->clearCheckpoints();
                db->clearSessions();
                db->clearAllWorkingState();
                db->clearRecoverySessions();
            }
            return true;
        }
        return true; // unknown kind
    }

    // For soft/mixed: commits between newHead and oldHead are unwound.
    // We need to recover their notes as working state.
    // Get the range of unwound commits
    std::string cmd = "git rev-list " + newHeadSha + ".." + oldHeadSha;
    std::string out = runGitCommand(cmd);
    if (out.empty()) return true;

    std::istringstream stream(out);
    std::string sha;
    std::vector<std::string> unwound;
    while (std::getline(stream, sha)) {
        if (!sha.empty()) unwound.push_back(sha);
    }

    auto* db = persist::getRepoDb(repoRoot);
    if (!db) return false;

    for (const auto& s : unwound) {
        auto idx = commit::NoteIndex::get(repoRoot, s);
        if (!idx.has_value() || !idx->note_exists) continue;

        // Read the note content
        std::string noteContent = readNote(repoRoot, "refs/notes/ghost", s);
        if (noteContent.empty()) continue;

        // Parse sessions from note content and create recovery sessions
        // We store the raw note as a recovery session keyed by commit SHA
        db->saveRecoverySession(s, noteContent);
    }

    return true;
}

bool Processor::detectStashPop(const std::string& repoRoot,
                               const std::string& prevHead,
                               const std::string& newHead) {
    // Stash pop: HEAD doesn't change, but working tree changes
    // We detect by checking if a saved stash state exists
    (void)prevHead;
    (void)newHead;

    if (WorkingState::exists(repoRoot, "stash")) {
        WorkingState::restore(repoRoot, "stash");
        WorkingState::clear(repoRoot, "stash");
        return true;
    }
    return false;
}

// --- Generic helpers ---

bool Processor::copyNote(const std::string& repoRoot,
                         const std::string& fromRef,
                         const std::string& fromSha,
                         const std::string& toSha) {
    std::string cmd = "git -C \"" + repoRoot + "\" notes --ref=" + fromRef +
                       " copy " + fromSha + " " + toSha + " 2>&1";
    std::string out = runGitCommand(cmd);
    // git notes copy returns empty on success, error message on failure
    // Also success if note already exists on target
    return out.empty() || out.find("already exists") != std::string::npos;
}

std::string Processor::readNote(const std::string& repoRoot,
                                  const std::string& ref,
                                  const std::string& sha) {
    std::string cmd = "git -C \"" + repoRoot + "\" notes --ref=" + ref +
                       " show " + sha + " 2>/dev/null";
    return runGitCommand(cmd);
}

bool Processor::writeNote(const std::string& repoRoot,
                          const std::string& ref,
                          const std::string& sha,
                          const std::string& content) {
    // Write via temp file (same approach as git::Notes::write)
    std::string tmpPath = repoRoot + "/.git/ghost/note-tmp.txt";
    {
        std::FILE* f = std::fopen(tmpPath.c_str(), "w");
        if (!f) return false;
        std::fwrite(content.c_str(), 1, content.size(), f);
        std::fclose(f);
    }
    std::string cmd = "git -C \"" + repoRoot + "\" notes --ref=" + ref +
                       " add -f -F \"" + tmpPath + "\" " + sha;
    int rc = std::system(cmd.c_str());
    std::remove(tmpPath.c_str());
    return rc == 0;
}

} // namespace rewrite
} // namespace ghost
