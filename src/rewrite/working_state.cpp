#include "working_state.hpp"
#include "persist/db.hpp"
#include <sstream>
#include <iostream>

namespace ghost {
namespace rewrite {

bool WorkingState::save(const std::string& repoRoot, const std::string& key) {
    auto* db = persist::getRepoDb(repoRoot);
    if (!db) return false;

    // Save checkpoints
    auto checkpoints = db->loadCheckpoints(true);
    if (!checkpoints.empty()) {
        std::ostringstream oss;
        oss << "[\"checkpoints\":";
        for (size_t i = 0; i < checkpoints.size(); ++i) {
            if (i > 0) oss << ",";
            oss << "{\"agent\":\"" << checkpoints[i].agent << "\",";
            oss << "\"model\":\"" << checkpoints[i].model << "\",";
            oss << "\"target_file\":\"" << checkpoints[i].target_file << "\",";
            oss << "\"snapshot_path\":\"" << checkpoints[i].snapshot_path << "\",";
            oss << "\"ts_start\":" << checkpoints[i].ts_start << "}";
        }
        oss << "]";
        db->saveWorkingState("checkpoints_" + key, oss.str());
    }

    // Save sessions
    auto sessions = db->loadSessions(true);
    if (!sessions.empty()) {
        std::ostringstream oss;
        oss << "[\"sessions\":";
        for (size_t i = 0; i < sessions.size(); ++i) {
            if (i > 0) oss << ",";
            oss << "{\"session_id\":\"" << sessions[i].session_id << "\",";
            oss << "\"json_data\":\"" << sessions[i].json_data << "\"}";
        }
        oss << "]";
        db->saveWorkingState("sessions_" + key, oss.str());
    }

    return true;
}

bool WorkingState::restore(const std::string& repoRoot, const std::string& key) {
    auto* db = persist::getRepoDb(repoRoot);
    if (!db) return false;

    // Restore checkpoints
    auto cpJson = db->loadWorkingState("checkpoints_" + key);
    if (cpJson.has_value()) {
        // For now, we don't auto-restore checkpoints because they reference
        // snapshot files that may no longer exist. Instead, we restore sessions.
        (void)cpJson;
    }

    // Restore sessions
    auto sessJson = db->loadWorkingState("sessions_" + key);
    if (sessJson.has_value()) {
        // The session JSON is already stored in the sessions table via saveSessionsJson
        // This is handled by restoreSessionsJson
    }

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
    if (sessions.empty()) return true; // nothing to save

    std::ostringstream oss;
    oss << "[\"sessions\":";
    for (size_t i = 0; i < sessions.size(); ++i) {
        if (i > 0) oss << ",";
        oss << sessions[i].json_data;
    }
    oss << "]";

    return db->saveWorkingState("sessions_" + key, oss.str());
}

bool WorkingState::restoreSessionsJson(const std::string& repoRoot, const std::string& key) {
    auto* db = persist::getRepoDb(repoRoot);
    if (!db) return false;

    auto data = db->loadWorkingState("sessions_" + key);
    if (!data.has_value()) return false;

    const std::string& json = data.value();
    // Parse the array of session JSON objects
    // Format: [{"sessions":<sess1_json>,<sess2_json>,...}]
    // We store them as a flat array of session JSONs
    size_t pos = 0;
    while ((pos = json.find("{\"session_id\":\"", pos)) != std::string::npos) {
        size_t end = json.find("\"ts_end\":", pos);
        if (end == std::string::npos) break;
        // Find the end of this object (next object start or array end)
        size_t objEnd = json.find("}", end);
        // Count braces to find proper end
        int braceCount = 1;
        objEnd = pos + 1;
        while (objEnd < json.size() && braceCount > 0) {
            if (json[objEnd] == '{') braceCount++;
            else if (json[objEnd] == '}') braceCount--;
            objEnd++;
        }
        std::string sessJson = json.substr(pos, objEnd - pos);

        // Extract session fields to save to DB
        persist::Session sess;
        size_t sp = sessJson.find("\"session_id\":\"");
        if (sp != std::string::npos) {
            sp += 15;
            size_t ep = sessJson.find("\"", sp);
            if (ep != std::string::npos) sess.session_id = sessJson.substr(sp, ep - sp);
        }
        sp = sessJson.find("\"agent\":\"");
        if (sp != std::string::npos) {
            sp += 9;
            size_t ep = sessJson.find("\"", sp);
            if (ep != std::string::npos) sess.agent = sessJson.substr(sp, ep - sp);
        }
        sp = sessJson.find("\"model\":\"");
        if (sp != std::string::npos) {
            sp += 9;
            size_t ep = sessJson.find("\"", sp);
            if (ep != std::string::npos) sess.model = sessJson.substr(sp, ep - sp);
        }
        sp = sessJson.find("\"author\":\"");
        if (sp != std::string::npos) {
            sp += 10;
            size_t ep = sessJson.find("\"", sp);
            if (ep != std::string::npos) sess.author = sessJson.substr(sp, ep - sp);
        }
        sp = sessJson.find("\"ts_start\":");
        if (sp != std::string::npos) {
            sp += 11;
            size_t ep = sessJson.find_first_of(",}", sp);
            try { sess.ts_start = std::stoll(sessJson.substr(sp, ep - sp)); } catch (...) {}
        }
        sp = sessJson.find("\"ts_end\":");
        if (sp != std::string::npos) {
            sp += 9;
            size_t ep = sessJson.find_first_of(",}", sp);
            try { sess.ts_end = std::stoll(sessJson.substr(sp, ep - sp)); } catch (...) {}
        }
        sp = sessJson.find("\"additions\":");
        if (sp != std::string::npos) {
            sp += 12;
            size_t ep = sessJson.find_first_of(",}", sp);
            try { sess.additions = std::stoi(sessJson.substr(sp, ep - sp)); } catch (...) {}
        }
        sp = sessJson.find("\"deletions\":");
        if (sp != std::string::npos) {
            sp += 12;
            size_t ep = sessJson.find_first_of(",}", sp);
            try { sess.deletions = std::stoi(sessJson.substr(sp, ep - sp)); } catch (...) {}
        }
        sess.json_data = sessJson;

        db->saveSession(sess);
        pos = objEnd;
    }

    return true;
}

} // namespace rewrite
} // namespace ghost
