#include "session_summary.hpp"
#include "checkpoint/session_json.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace ghost {
namespace cli {
namespace SessionSummary {
namespace {

static std::string canonicalPathString(const std::string& path) {
    std::error_code ec;
    auto canonical = fs::weakly_canonical(fs::path(path), ec);
    return (ec ? fs::path(path) : canonical).string();
}

static bool samePath(const std::string& a, const std::string& b) {
#ifdef _WIN32
    std::string left = canonicalPathString(a);
    std::string right = canonicalPathString(b);
    std::transform(left.begin(), left.end(), left.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(right.begin(), right.end(), right.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return left == right;
#else
    return canonicalPathString(a) == canonicalPathString(b);
#endif
}

static std::string findRepoRootForPath(const std::string& path) {
    if (path.empty()) return "";
    std::error_code ec;
    fs::path p(path);
    if (!p.is_absolute()) {
        p = fs::absolute(p, ec);
        if (ec) return "";
    }
    fs::path dir = fs::is_directory(p, ec) ? p : p.parent_path();
    while (!dir.empty()) {
        if (fs::exists(dir / ".git", ec)) {
            return dir.string();
        }
        auto parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return "";
}

static bool belongsToRepo(const persist::Session& session, const std::string& repoRoot) {
    auto sessionFiles = files(session.json_data, repoRoot);
    auto parsed = checkpoint::SessionJson::parse(session.json_data);
    if (parsed && !parsed->entries.empty() && sessionFiles.empty()) return false;
    if (sessionFiles.empty()) return true;
    for (const auto& file : sessionFiles) {
        std::string owner = findRepoRootForPath((fs::path(repoRoot) / file).string());
        if (!owner.empty() && !samePath(owner, repoRoot)) {
            return false;
        }
    }
    return true;
}

static std::string fingerprintForDisplay(const persist::Session& session, const std::string& repoRoot) {
    checkpoint::CapturedSession captured;
    if (auto parsed = checkpoint::SessionJson::parse(session.json_data)) {
        captured = *parsed;
    }
    captured.db_id = session.id;
    captured.session_id = session.session_id;
    captured.agent = session.agent;
    captured.model = session.model;
    captured.author = session.author;
    captured.ts_start = session.ts_start;
    captured.ts_end = session.ts_end;
    captured.additions = session.additions;
    captured.deletions = session.deletions;
    return checkpoint::SessionJson::fingerprint(captured, repoRoot);
}

}

std::vector<std::string> files(const std::string& jsonData, const std::string& repoRoot) {
    return checkpoint::SessionJson::files(jsonData, repoRoot);
}

note::LineRangeSet rangesForFile(const std::string& jsonData, const std::string& filePath, const std::string& repoRoot) {
    return checkpoint::SessionJson::rangesForFile(jsonData, filePath, repoRoot);
}

void normalizePending(std::vector<persist::Session>& sessions, const std::string& repoRoot) {
    sessions.erase(
        std::remove_if(sessions.begin(), sessions.end(),
            [&](const auto& session) { return !belongsToRepo(session, repoRoot); }),
        sessions.end()
    );

    std::vector<persist::Session> unique;
    std::set<std::string> seenIds;
    std::set<std::string> seenFingerprints;
    for (const auto& session : sessions) {
        if (!session.session_id.empty() && !seenIds.insert(session.session_id).second) continue;

        std::string fingerprint = fingerprintForDisplay(session, repoRoot);
        if (!fingerprint.empty() && !seenFingerprints.insert(fingerprint).second) continue;

        unique.push_back(session);
    }
    sessions = std::move(unique);
}

std::vector<persist::Session> loadPending(const std::string& repoRoot) {
    std::vector<persist::Session> sessions;
    auto* db = persist::getRepoDb(repoRoot);
    if (db) {
        sessions = db->loadSessions(true);
    }
    normalizePending(sessions, repoRoot);
    return sessions;
}

}
}
}
