#include "post_commit.hpp"
#include "writer.hpp"
#include "verified_writer.hpp"
#include "line_range.hpp"
#include "notes.hpp"
#include "repo.hpp"
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

static void writeVerifiedNote(const std::string& repoRoot, const std::string& commitSha, int sessionCount) {
    note::VerifiedNote vnote;
    vnote.schema = "ghost-verified/1.0.0";
    vnote.ghost_version = GHOST_VERSION;
    vnote.commit = commitSha;
    vnote.ts = std::time(nullptr);
    vnote.author = getGitAuthor(repoRoot);
    vnote.sessions = sessionCount;

    std::string content = note::VerifiedWriter::write(vnote);
    git::Notes::write("refs/notes/ghost-verified", commitSha, content);
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
    for (const auto& session : sessions) {
        if (session.session_id.empty() || seenSessionIds.insert(session.session_id).second) {
            uniqueSessions.push_back(session);
        }
    }
    sessions = std::move(uniqueSessions);

    int sessionCount = static_cast<int>(sessions.size());

    std::set<std::string> commitFiles = getCommitChangedFiles(repoRoot, commitSha);

    if (sessionCount > 0) {
        std::vector<note::AuthorshipEntry> entries;
        std::map<std::string, note::Session> sessionMap;

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
            sessionMap[s.session_id] = sess;

            for (const auto& e : s.entries) {
                // Normalize session entry path to relative for comparison
                std::string entryPath = e.first;
                fs::path p(entryPath);
                if (p.is_absolute()) {
                    std::error_code ec;
                    fs::path rel = fs::relative(p, repoRoot, ec);
                    if (!ec) entryPath = rel.string();
                }
                for (char& c : entryPath) if (c == '\\') c = '/';
                if (commitFiles.find(entryPath) == commitFiles.end()) continue;

                note::AuthorshipEntry entry;
                entry.file_path = entryPath;
                entry.session_id = s.session_id;
                if (!e.second.empty()) {
                    entry.ranges = note::LineRangeSet::parse(e.second);
                }
                entries.push_back(entry);
            }
        }

        if (!entries.empty()) {
            std::string noteContent = note::NoteWriter::write(entries, sessionMap, commitSha);
            git::Notes::write("refs/notes/ghost", commitSha, noteContent);
        }
    }

    writeVerifiedNote(repoRoot, commitSha, sessionCount);

    // Update note index
    bool hasGhostNote = sessionCount > 0;
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
