#include "gitai_reader.hpp"
#include <algorithm>
#include <sstream>
#include <set>

namespace ghost {
namespace note {

static std::string extractString(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\": \"";
    size_t start = json.find(search);
    if (start == std::string::npos) {
        search = "\"" + key + "\":\"";
        start = json.find(search);
        if (start == std::string::npos) return "";
    }
    start += search.length();

    std::string value;
    bool escaped = false;
    for (size_t i = start; i < json.size(); ++i) {
        char c = json[i];
        if (escaped) {
            value += c;
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '"') break;
        value += c;
    }
    return value;
}

static long long extractNumber(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":";
    size_t start = json.find(search);
    if (start == std::string::npos) {
        search = "\"" + key + "\": ";
        start = json.find(search);
        if (start == std::string::npos) return 0;
    }
    start += search.length();
    while (start < json.size() && (json[start] == ' ' || json[start] == '\t')) start++;
    size_t end = json.find_first_of(",}]\n\r", start);
    if (end == std::string::npos) end = json.size();
    try {
        return std::stoll(json.substr(start, end - start));
    } catch (...) {
        return 0;
    }
}

static std::string findObjectForKey(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    size_t keyPos = json.find(search);
    if (keyPos == std::string::npos) return "";

    size_t objStart = json.find("{", keyPos + search.size());
    if (objStart == std::string::npos) return "";

    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (size_t i = objStart; i < json.size(); ++i) {
        char c = json[i];
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
                return json.substr(objStart, i - objStart + 1);
            }
        }
    }
    return "";
}

NoteReader::Result GitAiReader::parse(const std::string& note_content) {
    NoteReader::Result result;
    result.success = false;

    if (note_content.empty()) {
        result.error = "Empty note content";
        return result;
    }

    size_t separatorPos = note_content.find("---");
    if (separatorPos == std::string::npos) {
        result.error = "Missing separator";
        return result;
    }

    std::string topSection = note_content.substr(0, separatorPos);
    std::string jsonSection = note_content.substr(separatorPos + 3);

    std::istringstream topStream(topSection);
    std::string line;
    std::string currentFile;
    std::set<std::string> sessionIds;

    while (std::getline(topStream, line)) {
        if (line.empty()) continue;

        if (line[0] != ' ' && line[0] != '\t') {
            currentFile = line;
            continue;
        }

        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        std::string trimmed = line.substr(start);
        size_t spacePos = trimmed.find_first_of(" \t");
        if (spacePos == std::string::npos) continue;

        std::string sessionId = trimmed.substr(0, spacePos);
        size_t rangesStart = trimmed.find_first_not_of(" \t", spacePos);
        if (rangesStart == std::string::npos) continue;
        std::string rangesStr = trimmed.substr(rangesStart);

        AuthorshipEntry entry;
        entry.file_path = currentFile;
        entry.session_id = sessionId;
        entry.ranges = LineRangeSet::parse(rangesStr);
        result.entries.push_back(entry);
        result.entries_by_file[entry.file_path].push_back(entry);
        sessionIds.insert(sessionId);
    }

    result.commit_sha = extractString(jsonSection, "commit_sha");
    if (result.commit_sha.empty()) {
        result.commit_sha = extractString(jsonSection, "commit");
    }
    if (result.commit_sha.empty()) {
        result.commit_sha = extractString(jsonSection, "base_commit_sha");
    }

    for (const auto& sessionId : sessionIds) {
        std::string promptJson = findObjectForKey(jsonSection, sessionId);

        Session sess;
        sess.session_id = sessionId;
        sess.agent = extractString(promptJson, "tool");
        sess.model = extractString(promptJson, "model");
        sess.author = extractString(promptJson, "human_author");
        sess.ts_start = static_cast<time_t>(extractNumber(promptJson, "ts_start"));
        sess.ts_end = static_cast<time_t>(extractNumber(promptJson, "ts_end"));
        sess.additions = static_cast<int>(extractNumber(promptJson, "total_additions"));
        sess.deletions = static_cast<int>(extractNumber(promptJson, "total_deletions"));
        mapToGhostFormat(sess);
        result.sessions[sessionId] = sess;
    }

    result.success = true;
    return result;
}

void GitAiReader::mapToGhostFormat(Session& session) {
    if (session.agent.empty()) session.agent = "ai";
    if (session.model.empty()) session.model = "unknown";
    if (session.author.empty()) session.author = "unknown";
}

}
}
