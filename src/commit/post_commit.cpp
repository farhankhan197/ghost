#include "post_commit.hpp"
#include "writer.hpp"
#include "verified_writer.hpp"
#include "line_range.hpp"
#include "notes.hpp"
#include "repo.hpp"
#include "diff.hpp"
#include "path.hpp"
#include "working_log.hpp"
#include "note_index.hpp"
#include "persist/db.hpp"
#include <fstream>
#include <sstream>
#include <vector>
#include <map>
#include <set>
#include <ctime>
#include <cstdio>
#include <memory>
#include <filesystem>
#include <iostream>
#include <algorithm>

namespace fs = std::filesystem;

namespace ghost {
namespace commit {

static std::string runCommand(const std::string& cmd) {
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) return "";
    std::string result;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe.get())) result += buffer;
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) result.pop_back();
    return result;
}

static std::set<std::string> getCommitChangedFiles(const std::string& repoRoot, const std::string& commitSha) {
    (void)repoRoot;
    std::string output = runCommand("git diff-tree --root --no-commit-id -r --name-only " + commitSha + " -- .");
    std::set<std::string> files;
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty()) files.insert(line);
    }
    return files;
}

static std::string extractString(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\": \"";
    size_t start = json.find(search);
    if (start == std::string::npos) {
        search = "\"" + key + "\":\"";
        start = json.find(search);
        if (start == std::string::npos) return "";
        start += search.length();
    } else {
        start += search.length();
    }
    size_t end = json.find("\"", start);
    if (end == std::string::npos) return "";
    return json.substr(start, end - start);
}

static long long extractNumber(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":";
    size_t start = json.find(search);
    if (start == std::string::npos) return 0;
    start += search.length();
    size_t end = json.find_first_of(",}]", start);
    if (end == std::string::npos) return 0;
    try {
        return std::stoll(json.substr(start, end - start));
    } catch (...) {
        return 0;
    }
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
    bool valid;
};

static ParsedSession parseSessionFile(const std::string& path) {
    ParsedSession result;
    result.valid = false;
    result.additions = 0;
    result.deletions = 0;
    result.ts_start = 0;
    result.ts_end = 0;

    std::ifstream file(path);
    if (!file.is_open()) return result;

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (content.empty()) return result;

    result.session_id = extractString(content, "session_id");
    result.agent = extractString(content, "agent");
    result.model = extractString(content, "model");
    result.author = extractString(content, "author");
    result.ts_start = static_cast<time_t>(extractNumber(content, "ts_start"));
    result.ts_end = static_cast<time_t>(extractNumber(content, "ts_end"));
    result.additions = static_cast<int>(extractNumber(content, "additions"));
    result.deletions = static_cast<int>(extractNumber(content, "deletions"));

    size_t entriesStart = content.find("\"entries\":");
    if (entriesStart == std::string::npos) { result.valid = true; return result; }

    size_t arrStart = content.find("[", entriesStart);
    size_t arrEnd = content.find("]", arrStart);
    if (arrStart == std::string::npos || arrEnd == std::string::npos) { result.valid = true; return result; }

    std::string arrStr = content.substr(arrStart, arrEnd - arrStart + 1);
    size_t pos = 0;
    while ((pos = arrStr.find("{", pos)) != std::string::npos) {
        size_t objEnd = arrStr.find("}", pos);
        if (objEnd == std::string::npos) break;
        std::string obj = arrStr.substr(pos, objEnd - pos + 1);
        std::string filePath = extractString(obj, "file_path");
        std::string ranges = extractString(obj, "ranges");
        if (!filePath.empty()) {
            result.entries.push_back({filePath, ranges});
        }
        pos = objEnd + 1;
    }

    result.valid = true;
    return result;
}

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

static void cleanupSessions(const std::string& repoRoot) {
    std::string sessionsDir = (fs::path(checkpoint::WorkingLog::getGhostDir(repoRoot)) / "sessions").string();
    std::error_code ec;
    if (fs::exists(sessionsDir, ec)) {
        for (const auto& entry : fs::directory_iterator(sessionsDir, ec)) {
            fs::remove(entry.path(), ec);
        }
    }
}

static std::string getGitAuthor(const std::string& repoRoot) {
    (void)repoRoot;
    std::string name, email;
    std::unique_ptr<FILE, decltype(&pclose)> namePipe(popen("git config user.name", "r"), pclose);
    if (namePipe) {
        char buf[128];
        while (fgets(buf, sizeof(buf), namePipe.get())) name += buf;
    }
    std::unique_ptr<FILE, decltype(&pclose)> emailPipe(popen("git config user.email", "r"), pclose);
    if (emailPipe) {
        char buf[128];
        while (fgets(buf, sizeof(buf), emailPipe.get())) email += buf;
    }
    while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) name.pop_back();
    while (!email.empty() && (email.back() == '\n' || email.back() == '\r')) email.pop_back();
    if (!name.empty() && !email.empty()) return name + " <" + email + ">";
    return "unknown";
}

static std::string hashFile(const std::string& path) {
    return runCommand("git hash-object \"" + path + "\" 2>&1");
}

static std::string hashText(const std::string& repoRoot, const std::string& content) {
    std::error_code ec;
    fs::path tmpDir = fs::path(repoRoot) / ".git" / "ghost";
    fs::create_directories(tmpDir, ec);
    fs::path tmpPath = tmpDir / ("hash-" + std::to_string(std::time(nullptr)) + ".txt");
    {
        std::ofstream out(tmpPath);
        if (!out.is_open()) return "";
        out << content;
    }
    std::string digest = hashFile(tmpPath.string());
    fs::remove(tmpPath, ec);
    return digest;
}

static void writeNoteSignature(
    const std::string& repoRoot,
    const std::string& commitSha,
    const std::string& ghostNote,
    const std::string& verifiedNote
) {
    std::string signer = getGitAuthor(repoRoot);
    std::string ghostDigest = ghostNote.empty() ? "absent" : hashText(repoRoot, ghostNote);
    std::string verifiedDigest = verifiedNote.empty() ? "absent" : hashText(repoRoot, verifiedNote);

    std::ostringstream sig;
    sig << "schema: ghost-note-signature/1\n";
    sig << "commit: " << commitSha << "\n";
    sig << "ghost_digest: " << ghostDigest << "\n";
    sig << "verified_digest: " << verifiedDigest << "\n";
    sig << "signer: " << (signer.empty() ? "unknown" : signer) << "\n";
    sig << "ts: " << std::time(nullptr) << "\n";
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
    std::string sessionsDir = (fs::path(checkpoint::WorkingLog::getGhostDir(repoRoot)) / "sessions").string();
    std::error_code ec;

    // --- Load legacy file-based sessions ---
    std::vector<std::string> sessionFiles = checkpoint::WorkingLog::listSessions(repoRoot);
    std::vector<ParsedSession> sessions;

    for (const auto& file : sessionFiles) {
        std::string fullPath = (fs::path(sessionsDir) / file).string();
        ParsedSession parsed = parseSessionFile(fullPath);
        if (parsed.valid) {
            sessions.push_back(parsed);
        } else {
            std::cerr << "Warning: failed to parse session " << file << "\n";
        }
    }

    // --- Load DB-based sessions (uncommitted) ---
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
            parsed.valid = true;

            // Parse entries from json_data
            size_t pos = s.json_data.find("\"entries\":");
            if (pos != std::string::npos) {
                pos = s.json_data.find("[", pos);
                size_t arrEnd = s.json_data.find("]", pos);
                if (pos != std::string::npos && arrEnd != std::string::npos) {
                    std::string arr = s.json_data.substr(pos, arrEnd - pos + 1);
                    size_t p = 0;
                    while ((p = arr.find("{", p)) != std::string::npos) {
                        size_t objEnd = arr.find("}", p);
                        if (objEnd == std::string::npos) break;
                        std::string obj = arr.substr(p, objEnd - p + 1);
                        std::string fp = extractString(obj, "file_path");
                        std::string rng = extractString(obj, "ranges");
                        if (!fp.empty()) {
                            parsed.entries.push_back({fp, rng});
                        }
                        p = objEnd + 1;
                    }
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

    std::vector<ParsedSession> uniqueSessions;
    std::set<std::string> seenSessionIds;
    std::set<std::string> seenFingerprints;
    for (const auto& session : sessions) {
        if (session.session_id.empty()) continue;
        if (!seenSessionIds.insert(session.session_id).second) continue;

        std::string fingerprint = sessionFingerprint(session, repoRoot);
        if (!fingerprint.empty() && !seenFingerprints.insert(fingerprint).second) continue;

        uniqueSessions.push_back(session);
    }
    sessions = std::move(uniqueSessions);

    int sessionCount = static_cast<int>(sessions.size());

    std::set<std::string> commitFiles = getCommitChangedFiles(repoRoot, commitSha);
    git::DiffRanges commitRanges = git::Diff::getCommitRanges(repoRoot, commitSha);

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
            git::Notes::write("refs/notes/ghost", commitSha, noteContent);
        }
    }

    (void)writeVerifiedNote(repoRoot, commitSha, sessionCount);
    std::string storedGhostNote = git::Notes::show("refs/notes/ghost", commitSha);
    std::string storedVerifiedNote = git::Notes::show("refs/notes/ghost-verified", commitSha);
    writeNoteSignature(repoRoot, commitSha, storedGhostNote, storedVerifiedNote);

    // Update note index
    bool hasGhostNote = !storedGhostNote.empty();
    NoteIndex::update(repoRoot, commitSha, "refs/notes/ghost", hasGhostNote, sessionCount);

    cleanupSessions(repoRoot);

    // Mark DB sessions as committed and clear recovery
    if (db) {
        auto dbSessions = db->loadSessions(true);
        for (const auto& s : dbSessions) {
            db->markSessionCommitted(s.id);
        }
        db->clearRecoverySessions();
        db->clearCheckpoints();
    }

    return 0;
}

}
}
