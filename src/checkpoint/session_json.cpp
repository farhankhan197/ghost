#include "session_json.hpp"
#include "git/path.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <sstream>

namespace ghost {
namespace checkpoint {
namespace {

using Json = nlohmann::json;

std::string normalizedPath(const std::string& path, const std::string& repoRoot) {
    return git::Path::normalizeRepoPathOrEmpty(path, repoRoot);
}

int lineCount(const std::vector<SessionEntry>& entries) {
    int count = 0;
    for (const auto& entry : entries) {
        try {
            count += static_cast<int>(note::LineRangeSet::parse(entry.ranges).lineCount());
        } catch (...) {
        }
    }
    return count;
}

}

std::string SessionJson::write(const CapturedSession& session) {
    Json json;
    json["session_id"] = session.session_id;
    json["agent"] = session.agent;
    json["model"] = session.model;
    json["author"] = session.author;
    json["ts_start"] = static_cast<long long>(session.ts_start);
    json["ts_end"] = static_cast<long long>(session.ts_end);
    json["additions"] = session.additions > 0 ? session.additions : lineCount(session.entries);
    json["deletions"] = session.deletions;
    json["entries"] = Json::array();
    for (const auto& entry : session.entries) {
        Json item;
        item["file_path"] = entry.file_path;
        item["ranges"] = entry.ranges;
        json["entries"].push_back(item);
    }
  return json.dump(2);
}

std::optional<CapturedSession> SessionJson::parse(const std::string& jsonText) {
    if (jsonText.empty()) return std::nullopt;
    Json json;
    try {
        json = Json::parse(jsonText);
    } catch (...) {
        return std::nullopt;
    }
    if (!json.is_object()) return std::nullopt;

    CapturedSession session;
    session.session_id = json.value("session_id", "");
    session.agent = json.value("agent", "");
    session.model = json.value("model", "");
    session.author = json.value("author", "");
    session.ts_start = static_cast<time_t>(json.value("ts_start", 0LL));
    session.ts_end = static_cast<time_t>(json.value("ts_end", 0LL));
    session.additions = json.value("additions", 0);
    session.deletions = json.value("deletions", 0);

    auto entriesIt = json.find("entries");
    if (entriesIt != json.end() && entriesIt->is_array()) {
        for (const auto& item : *entriesIt) {
            if (!item.is_object()) continue;
            SessionEntry entry;
            entry.file_path = item.value("file_path", "");
            entry.ranges = item.value("ranges", "");
            if (!entry.file_path.empty()) {
                session.entries.push_back(entry);
            }
        }
    }

    if (session.additions == 0) {
        session.additions = lineCount(session.entries);
    }
    return session;
}

std::vector<std::string> SessionJson::files(const std::string& jsonText, const std::string& repoRoot) {
    std::vector<std::string> result;
    auto parsed = parse(jsonText);
    if (!parsed) return result;
    for (const auto& entry : parsed->entries) {
        std::string file = normalizedPath(entry.file_path, repoRoot);
        if (!file.empty() && std::find(result.begin(), result.end(), file) == result.end()) {
            result.push_back(file);
        }
    }
    return result;
}

note::LineRangeSet SessionJson::rangesForFile(const std::string& jsonText, const std::string& filePath, const std::string& repoRoot) {
    note::LineRangeSet result;
    auto parsed = parse(jsonText);
    if (!parsed) return result;
    std::string target = normalizedPath(filePath, repoRoot);
    if (target.empty()) return result;

    for (const auto& entry : parsed->entries) {
        std::string file = normalizedPath(entry.file_path, repoRoot);
        if (file != target) continue;
        try {
            result = result.unite(note::LineRangeSet::parse(entry.ranges));
        } catch (...) {
        }
    }
    return result;
}

std::string SessionJson::fingerprint(const CapturedSession& session, const std::string& repoRoot) {
    std::vector<std::pair<std::string, std::string>> entries;
    for (const auto& entry : session.entries) {
        std::string file = normalizedPath(entry.file_path, repoRoot);
        if (!file.empty()) {
            entries.push_back({file, entry.ranges});
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

}
}
