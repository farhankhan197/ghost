#include "working_state.hpp"

#include "checkpoint/session_json.hpp"
#include "persist/db.hpp"

#include <nlohmann/json.hpp>

namespace ghost {
namespace rewrite {
namespace {

using Json = nlohmann::json;

Json checkpointToJson(const persist::Checkpoint& checkpoint) {
    return Json{
        {"agent", checkpoint.agent},
        {"model", checkpoint.model},
        {"target_file", checkpoint.target_file},
        {"snapshot_path", checkpoint.snapshot_path},
        {"ts_start", static_cast<long long>(checkpoint.ts_start)}
    };
}

checkpoint::CapturedSession capturedFromPersist(const persist::Session& session) {
    checkpoint::CapturedSession captured;
    captured.db_id = session.id;
    captured.session_id = session.session_id;
    captured.agent = session.agent;
    captured.model = session.model;
    captured.author = session.author;
    captured.ts_start = session.ts_start;
    captured.ts_end = session.ts_end;
    captured.additions = session.additions;
    captured.deletions = session.deletions;
    captured.committed = session.committed;
    return captured;
}

Json sessionToJson(const persist::Session& session) {
    try {
        Json parsed = Json::parse(session.json_data);
        if (parsed.is_object()) {
            if (!parsed.contains("session_id")) parsed["session_id"] = session.session_id;
            if (!parsed.contains("agent")) parsed["agent"] = session.agent;
            if (!parsed.contains("model")) parsed["model"] = session.model;
            if (!parsed.contains("author")) parsed["author"] = session.author;
            if (!parsed.contains("ts_start")) parsed["ts_start"] = static_cast<long long>(session.ts_start);
            if (!parsed.contains("ts_end")) parsed["ts_end"] = static_cast<long long>(session.ts_end);
            if (!parsed.contains("additions")) parsed["additions"] = session.additions;
            if (!parsed.contains("deletions")) parsed["deletions"] = session.deletions;
            if (!parsed.contains("entries")) parsed["entries"] = Json::array();
            return parsed;
        }
    } catch (...) {
    }
    return Json::parse(checkpoint::SessionJson::write(capturedFromPersist(session)));
}

persist::Session persistFromCaptured(const checkpoint::CapturedSession& captured) {
    persist::Session session;
    session.id = captured.db_id;
    session.session_id = captured.session_id;
    session.agent = captured.agent.empty() ? "unknown" : captured.agent;
    session.model = captured.model.empty() ? "unknown" : captured.model;
    session.author = captured.author;
    session.ts_start = captured.ts_start;
    session.ts_end = captured.ts_end;
    session.additions = captured.additions;
    session.deletions = captured.deletions;
    session.json_data = checkpoint::SessionJson::write(captured);
    session.committed = false;
    return session;
}

bool restoreSessionJson(persist::Database* db, const std::string& jsonText) {
    auto captured = checkpoint::SessionJson::parse(jsonText);
    if (!db || !captured || captured->session_id.empty()) return false;
    return db->saveSession(persistFromCaptured(*captured)) > 0;
}

int restoreSessionsFromArray(persist::Database* db, const Json& sessions) {
    if (!db || !sessions.is_array()) return 0;
    int restored = 0;
    for (const auto& item : sessions) {
        std::string jsonText;
        if (item.is_string()) {
            jsonText = item.get<std::string>();
        } else if (item.is_object()) {
            jsonText = item.dump();
        }
        if (!jsonText.empty() && restoreSessionJson(db, jsonText)) {
            restored++;
        }
    }
    return restored;
}

int restoreNewFormat(persist::Database* db, const std::string& data) {
    try {
        Json root = Json::parse(data);
        if (root.is_object()) {
            auto it = root.find("sessions");
            if (it != root.end()) return restoreSessionsFromArray(db, *it);
        }
        if (root.is_array()) return restoreSessionsFromArray(db, root);
    } catch (...) {
    }
    return 0;
}

int restoreLegacyLooseSessions(persist::Database* db, const std::string& data) {
    int restored = 0;
    size_t pos = 0;
    while ((pos = data.find("{\"session_id\"", pos)) != std::string::npos) {
        int depth = 0;
        bool inString = false;
        bool escaped = false;
        size_t end = pos;
        for (; end < data.size(); ++end) {
            char c = data[end];
            if (escaped) {
                escaped = false;
                continue;
            }
            if (c == '\\') {
                escaped = true;
                continue;
            }
            if (c == '"') {
                inString = !inString;
                continue;
            }
            if (inString) continue;
            if (c == '{') depth++;
            if (c == '}') {
                depth--;
                if (depth == 0) {
                    end++;
                    break;
                }
            }
        }
        if (end <= pos || end > data.size()) break;
        if (restoreSessionJson(db, data.substr(pos, end - pos))) {
            restored++;
        }
        pos = end;
    }
    return restored;
}

} // namespace

bool WorkingState::save(const std::string& repoRoot, const std::string& key) {
    auto* db = persist::getRepoDb(repoRoot);
    if (!db) return false;

    auto checkpoints = db->loadCheckpoints(true);
    if (!checkpoints.empty()) {
        Json root;
        root["checkpoints"] = Json::array();
        for (const auto& checkpoint : checkpoints) {
            root["checkpoints"].push_back(checkpointToJson(checkpoint));
        }
        db->saveWorkingState("checkpoints_" + key, root.dump(2));
    }

    return saveSessionsJson(repoRoot, key);
}

bool WorkingState::restore(const std::string& repoRoot, const std::string& key) {
    auto* db = persist::getRepoDb(repoRoot);
    if (!db) return false;

    return restoreSessionsJson(repoRoot, key);
}

bool WorkingState::clear(const std::string& repoRoot, const std::string& key) {
    auto* db = persist::getRepoDb(repoRoot);
    if (!db) return false;
    db->deleteWorkingState("checkpoints_" + key);
    db->deleteWorkingState("sessions_" + key);
    return true;
}

bool WorkingState::exists(const std::string& repoRoot, const std::string& key) {
    auto* db = persist::getRepoDb(repoRoot);
    if (!db) return false;
    return db->loadWorkingState("sessions_" + key).has_value();
}

bool WorkingState::saveSessionsJson(const std::string& repoRoot, const std::string& key) {
    auto* db = persist::getRepoDb(repoRoot);
    if (!db) return false;

    auto sessions = db->loadSessions(true);
    if (sessions.empty()) return true;

    Json root;
    root["sessions"] = Json::array();
    for (const auto& session : sessions) {
        root["sessions"].push_back(sessionToJson(session));
    }

    return db->saveWorkingState("sessions_" + key, root.dump(2));
}

bool WorkingState::restoreSessionsJson(const std::string& repoRoot, const std::string& key) {
    auto* db = persist::getRepoDb(repoRoot);
    if (!db) return false;

    auto data = db->loadWorkingState("sessions_" + key);
    if (!data.has_value()) return false;

    int restored = restoreNewFormat(db, data.value());
    if (restored == 0) {
        restored = restoreLegacyLooseSessions(db, data.value());
    }
    return restored > 0;
}

} // namespace rewrite
} // namespace ghost
