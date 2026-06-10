#include "post_commit.hpp"
#include "writer.hpp"
#include "verified_writer.hpp"
#include "line_range.hpp"
#include "notes.hpp"
#include "repo.hpp"
#include "diff.hpp"
#include "path.hpp"
#include "engine.hpp"
#include "note_index.hpp"
#include "persist/db.hpp"
#include "config/ghost_config.hpp"
#include "checkpoint/session_json.hpp"
#include "signing/ssh_signing.hpp"
#include "util/process.hpp"
#include <fstream>
#include <sstream>
#include <vector>
#include <map>
#include <set>
#include <ctime>
#include <iostream>
#include <algorithm>

namespace ghost {
namespace commit {

static std::string runGit(const std::string& repoRoot, std::vector<std::string> args) {
    util::Process::Command command;
    command.executable = "git";
    command.args = std::move(args);
    command.cwd = repoRoot;
    return util::Process::capture(command).stdoutText;
}

static std::set<std::string> getCommitChangedFiles(const std::string& repoRoot, const std::string& commitSha) {
    std::string output = runGit(repoRoot, {"diff-tree", "--root", "--no-commit-id", "-r", "--name-only", commitSha, "--", "."});
    std::set<std::string> files;
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty()) files.insert(line);
    }
    return files;
}

struct ParsedSession {
    std::string session_id;
    std::string agent;
    std::string model;
    std::string author;
    time_t ts_start;
    time_t ts_end;
    int additions;
    int deletions;
    std::vector<std::pair<std::string, std::string>> entries;
    int db_id;
    bool has_db_source;
    bool valid;
};

static std::string sessionFingerprint(const ParsedSession& session, const std::string& repoRoot) {
    std::vector<std::pair<std::string, std::string>> entries;
    for (const auto& entry : session.entries) {
        std::string normalized = git::Path::normalizeRepoPathOrEmpty(entry.first, repoRoot);
        if (!normalized.empty()) {
            entries.push_back({normalized, entry.second});
        }
    }
    std::sort(entries.begin(), entries.end());

    std::ostringstream out;
    out << session.agent << "|"
        << session.model << "|"
        << session.author << "|"
        << session.ts_start << "|"
        << session.ts_end << "|";
    for (const auto& [path, ranges] : entries) {
        out << path << ":" << ranges << ";";
    }
    return out.str();
}

static int countSessionAdditions(const ParsedSession& session) {
    int additions = 0;
    for (const auto& entry : session.entries) {
        try {
            additions += static_cast<int>(note::LineRangeSet::parse(entry.second).lineCount());
        } catch (...) {}
    }
    return additions;
}

static std::string serializeSessionJson(const ParsedSession& session) {
    checkpoint::CapturedSession captured;
    captured.db_id = session.db_id;
    captured.session_id = session.session_id;
    captured.agent = session.agent;
    captured.model = session.model;
    captured.author = session.author;
    captured.ts_start = session.ts_start;
    captured.ts_end = session.ts_end;
    captured.additions = countSessionAdditions(session);
    captured.deletions = session.deletions;
    for (const auto& [file, ranges] : session.entries) {
        captured.entries.push_back({file, ranges});
    }
    return checkpoint::SessionJson::write(captured);
}

static std::string getGitAuthor(const std::string& repoRoot) {
    std::string name = runGit(repoRoot, {"config", "user.name"});
    std::string email = runGit(repoRoot, {"config", "user.email"});
    while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) name.pop_back();
    while (!email.empty() && (email.back() == '\n' || email.back() == '\r')) email.pop_back();
    if (!name.empty() && !email.empty()) return name + " <" + email + ">";
    return "unknown";
}

static std::string getGitEmail(const std::string& repoRoot) {
    std::string email = runGit(repoRoot, {"config", "user.email"});
    while (!email.empty() && (email.back() == '\n' || email.back() == '\r')) email.pop_back();
    return email;
}

static std::string hashText(const std::string& repoRoot, const std::string& content) {
    return git::Engine::hashObject(repoRoot, content);
}

static void writeNoteSignature(
    const std::string& repoRoot,
    const std::string& commitSha,
    const std::string& ghostNote,
    const std::string& verifiedNote
) {
    std::string signer = getGitEmail(repoRoot);
    std::string ghostDigest = ghostNote.empty() ? "absent" : hashText(repoRoot, ghostNote);
    std::string verifiedDigest = verifiedNote.empty() ? "absent" : hashText(repoRoot, verifiedNote);
    auto cfg = config::GhostConfigReader::load(repoRoot);
    long long ts = static_cast<long long>(std::time(nullptr));

    std::ostringstream sig;
    if (signing::hasTrustedSigners(cfg)) {
        std::string signerPrincipal = signer.empty() ? "unknown" : signer;
        std::string payload = signing::canonicalNotePayload(commitSha, ghostDigest, verifiedDigest, signerPrincipal, ts);
        auto signedPayload = signing::signPayload(repoRoot, "ghost-notes", payload, cfg);
        if (signedPayload.ok) {
            sig << "schema: ghost-note-signature/2\n";
            sig << "commit: " << commitSha << "\n";
            sig << "ghost_digest: " << ghostDigest << "\n";
            sig << "verified_digest: " << verifiedDigest << "\n";
            sig << "signer: " << signedPayload.signer << "\n";
            sig << "ts: " << ts << "\n";
            sig << "namespace: ghost-notes\n";
            sig << "key_fingerprint: " << signedPayload.key_fingerprint << "\n";
            sig << "payload_b64: " << signedPayload.payload_b64 << "\n";
            sig << "signature_b64: " << signedPayload.signature_b64 << "\n";
            git::Notes::write("refs/notes/ghost-signatures", commitSha, sig.str());
            return;
        }
    }
    sig << "schema: ghost-note-signature/1\n";
    sig << "commit: " << commitSha << "\n";
    sig << "ghost_digest: " << ghostDigest << "\n";
    sig << "verified_digest: " << verifiedDigest << "\n";
    sig << "signer: " << (signer.empty() ? "unknown" : signer) << "\n";
    sig << "ts: " << ts << "\n";
    git::Notes::write("refs/notes/ghost-signatures", commitSha, sig.str());
}

static std::string writeVerifiedNote(const std::string& repoRoot, const std::string& commitSha, int sessionCount) {
    note::VerifiedNote vnote;
    vnote.schema = "ghost-verified/1.0.0";
    vnote.ghost_version = GHOST_VERSION;
    vnote.commit = commitSha;
    vnote.ts = std::time(nullptr);
    vnote.author = getGitAuthor(repoRoot);
    vnote.sessions = sessionCount;

    std::string content = note::VerifiedWriter::write(vnote);
    git::Notes::write("refs/notes/ghost-verified", commitSha, content);
    return content;
}

int PostCommit::run(const std::string& repoRoot, const std::string& commitSha) {
    std::vector<ParsedSession> sessions;
    auto* db = persist::getRepoDb(repoRoot);
    if (db) {
        auto dbSessions = db->loadSessions(true);
        for (const auto& s : dbSessions) {
            ParsedSession parsed;
            parsed.session_id = s.session_id;
            parsed.agent = s.agent;
            parsed.model = s.model;
            parsed.author = s.author;
            parsed.ts_start = s.ts_start;
            parsed.ts_end = s.ts_end;
            parsed.additions = s.additions;
            parsed.deletions = s.deletions;
            parsed.db_id = s.id;
            parsed.has_db_source = true;
            parsed.valid = true;

            auto captured = checkpoint::SessionJson::parse(s.json_data);
            if (captured) {
                for (const auto& entry : captured->entries) {
                    parsed.entries.push_back({entry.file_path, entry.ranges});
                }
            }
            sessions.push_back(parsed);
        }

        // Also load recovery sessions (from reset --soft, etc.)
        auto recovery = db->loadRecoverySessions();
        for (const auto& [recSid, recJson] : recovery) {
            ParsedSession parsed;
            parsed.session_id = recSid; // use commit SHA as session ID for recovery
            parsed.agent = "recovery";
            parsed.model = "unknown";
            parsed.author = getGitAuthor(repoRoot);
            parsed.ts_start = std::time(nullptr);
            parsed.ts_end = std::time(nullptr);
            parsed.additions = 0;
            parsed.deletions = 0;
            parsed.db_id = -1;
            parsed.has_db_source = false;
            parsed.valid = true;

            // Parse entries from recovery note JSON
            // Recovery sessions store raw note content; extract entries
            size_t sep = recJson.find("---");
            if (sep != std::string::npos) {
                std::string top = recJson.substr(0, sep);
                std::istringstream tstream(top);
                std::string line;
                std::string currentFile;
                while (std::getline(tstream, line)) {
                    if (line.empty()) continue;
                    if (line[0] != ' ') {
                        currentFile = line;
                    } else {
                        std::string trimmed = line.substr(2);
                        size_t sp = trimmed.find(' ');
                        if (sp == std::string::npos) continue;
                        std::string sid = trimmed.substr(0, sp);
                        std::string rangesStr = trimmed.substr(sp + 1);
                        parsed.entries.push_back({currentFile, rangesStr});
                    }
                }
            }
            if (!parsed.entries.empty()) {
                sessions.push_back(parsed);
            }
        }
    }

    std::map<std::string, ParsedSession> mergedById;
    std::vector<std::string> sessionOrder;
    for (const auto& session : sessions) {
        if (session.session_id.empty()) continue;
        auto it = mergedById.find(session.session_id);
        if (it == mergedById.end()) {
            mergedById[session.session_id] = session;
            sessionOrder.push_back(session.session_id);
            continue;
        }
        if (session.has_db_source) {
            it->second.db_id = session.db_id;
            it->second.has_db_source = true;
        }
    }

    std::vector<ParsedSession> uniqueSessions;
    std::vector<ParsedSession> duplicateSessions;
    std::set<std::string> seenFingerprints;
    std::map<std::string, std::string> fingerprintOwners;
    for (const auto& sessionId : sessionOrder) {
        auto found = mergedById.find(sessionId);
        if (found == mergedById.end()) continue;
        const auto& session = found->second;
        std::string fingerprint = sessionFingerprint(session, repoRoot);
        if (!fingerprint.empty() && !seenFingerprints.insert(fingerprint).second) {
            duplicateSessions.push_back(session);
            continue;
        }
        if (!fingerprint.empty()) {
            fingerprintOwners[fingerprint] = session.session_id;
        }

        uniqueSessions.push_back(session);
    }
    sessions = std::move(uniqueSessions);

    int sessionCount = static_cast<int>(sessions.size());

    std::set<std::string> commitFiles = getCommitChangedFiles(repoRoot, commitSha);
    git::DiffRanges commitRanges = git::Diff::getCommitRanges(repoRoot, commitSha);
    std::map<std::string, std::map<std::string, note::LineRangeSet>> consumedRanges;
    bool ghostNoteWritten = false;

    if (sessionCount > 0) {
        std::map<std::pair<std::string, std::string>, note::LineRangeSet> entryRanges;
        std::map<std::string, note::Session> allSessions;
        std::set<std::string> usedSessionIds;

        for (const auto& s : sessions) {
            note::Session sess;
            sess.session_id = s.session_id;
            sess.agent = s.agent;
            sess.model = s.model;
            sess.author = s.author;
            sess.ts_start = s.ts_start;
            sess.ts_end = s.ts_end;
            sess.additions = s.additions;
            sess.deletions = s.deletions;
            allSessions[s.session_id] = sess;

            for (const auto& e : s.entries) {
                std::string entryPath = git::Path::normalizeRepoPathOrEmpty(e.first, repoRoot);
                if (commitFiles.find(entryPath) == commitFiles.end()) continue;
                auto commitRangeIt = commitRanges.added.find(entryPath);
                if (commitRangeIt == commitRanges.added.end() || commitRangeIt->second.empty()) continue;

                note::LineRangeSet sessionRanges;
                try {
                    sessionRanges = e.second.empty()
                        ? commitRangeIt->second
                        : note::LineRangeSet::parse(e.second).intersect(commitRangeIt->second);
                } catch (...) {
                    continue;
                }
                if (sessionRanges.empty()) continue;

                auto key = std::make_pair(entryPath, s.session_id);
                auto existing = entryRanges.find(key);
                if (existing == entryRanges.end()) {
                    entryRanges[key] = sessionRanges;
                } else {
                    existing->second = existing->second.unite(sessionRanges);
                }
                auto consumedIt = consumedRanges[s.session_id].find(entryPath);
                if (consumedIt == consumedRanges[s.session_id].end()) {
                    consumedRanges[s.session_id][entryPath] = sessionRanges;
                } else {
                    consumedIt->second = consumedIt->second.unite(sessionRanges);
                }
                usedSessionIds.insert(s.session_id);
            }
        }

        std::vector<note::AuthorshipEntry> entries;
        std::map<std::string, note::Session> sessionMap;
        std::map<std::string, int> attributedAdditions;
        for (const auto& [key, ranges] : entryRanges) {
            note::AuthorshipEntry entry;
            entry.file_path = key.first;
            entry.session_id = key.second;
            entry.ranges = ranges;
            entries.push_back(entry);
            attributedAdditions[key.second] += static_cast<int>(ranges.lineCount());
        }
        for (const auto& sessionId : usedSessionIds) {
            auto it = allSessions.find(sessionId);
            if (it != allSessions.end()) {
                auto attributed = attributedAdditions.find(sessionId);
                if (attributed != attributedAdditions.end()) {
                    it->second.additions = attributed->second;
                }
                sessionMap[sessionId] = it->second;
            }
        }

        if (!entries.empty()) {
            std::string noteContent = note::NoteWriter::write(entries, sessionMap, commitSha);
            ghostNoteWritten = git::Notes::write("refs/notes/ghost", commitSha, noteContent);
        }
    }

    (void)writeVerifiedNote(repoRoot, commitSha, sessionCount);
    std::string storedGhostNote = git::Notes::show("refs/notes/ghost", commitSha);
    std::string storedVerifiedNote = git::Notes::show("refs/notes/ghost-verified", commitSha);
    writeNoteSignature(repoRoot, commitSha, storedGhostNote, storedVerifiedNote);

    // Update note index
    bool hasGhostNote = !storedGhostNote.empty();
    NoteIndex::update(repoRoot, commitSha, "refs/notes/ghost", hasGhostNote, sessionCount);

    if (ghostNoteWritten) {
        for (const auto& duplicate : duplicateSessions) {
            auto ownerIt = fingerprintOwners.find(sessionFingerprint(duplicate, repoRoot));
            if (ownerIt == fingerprintOwners.end()) continue;
            if (consumedRanges.find(ownerIt->second) == consumedRanges.end()) continue;
            if (db && duplicate.has_db_source && duplicate.db_id >= 0) db->markSessionCommitted(duplicate.db_id);
        }

        for (auto& session : sessions) {
            auto consumedForSession = consumedRanges.find(session.session_id);
            if (consumedForSession == consumedRanges.end()) continue;

            std::vector<std::pair<std::string, std::string>> remainingEntries;
            for (const auto& entry : session.entries) {
                std::string normalizedPath = git::Path::normalizeRepoPathOrEmpty(entry.first, repoRoot);
                auto consumedFile = consumedForSession->second.find(normalizedPath);
                if (consumedFile == consumedForSession->second.end()) {
                    remainingEntries.push_back(entry);
                    continue;
                }

                try {
                    if (entry.second.empty()) {
                        continue;
                    }
                    auto original = note::LineRangeSet::parse(entry.second);
                    auto remaining = original.subtract(consumedFile->second);
                    if (!remaining.empty()) {
                        remainingEntries.push_back({entry.first, remaining.toString()});
                    }
                } catch (...) {
                    remainingEntries.push_back(entry);
                }
            }

            session.entries = remainingEntries;
            session.additions = countSessionAdditions(session);
            if (session.entries.empty()) {
                if (db && session.has_db_source && session.db_id >= 0) db->markSessionCommitted(session.db_id);
            } else {
                if (db && session.has_db_source) {
                    persist::Session updated;
                    updated.id = session.db_id;
                    updated.session_id = session.session_id;
                    updated.agent = session.agent;
                    updated.model = session.model;
                    updated.author = session.author;
                    updated.ts_start = session.ts_start;
                    updated.ts_end = session.ts_end;
                    updated.additions = session.additions;
                    updated.deletions = session.deletions;
                    updated.json_data = serializeSessionJson(session);
                    updated.committed = false;
                    db->saveSession(updated);
                }
            }
        }
    }

    // Clear recovery only after a note-consuming commit; pending sessions carry forward.
    if (db) {
        if (ghostNoteWritten) db->clearRecoverySessions();
        db->clearCheckpoints();
    }

    return 0;
}

}
}
