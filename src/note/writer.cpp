#include "writer.hpp"
#include <sstream>
#include <map>
#include <algorithm>
#include <cstdio>

namespace ghost {
namespace note {

static std::string escapeJson(const std::string& value) {
    std::string escaped;
    for (char c : value) {
        switch (c) {
            case '"': escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default: escaped += c; break;
        }
    }
    return escaped;
}

std::string NoteWriter::serializeTop(const std::vector<AuthorshipEntry>& entries) {
    if (entries.empty()) {
        return "";
    }

    std::map<std::string, std::vector<std::pair<std::string, std::string>>> fileSessions;

    for (const auto& entry : entries) {
        fileSessions[entry.file_path].push_back({
            entry.session_id,
            entry.ranges.toString()
        });
    }

    std::ostringstream oss;

    for (const auto& filePair : fileSessions) {
        oss << filePair.first << "\n";

        for (const auto& sessionPair : filePair.second) {
            oss << "  " << sessionPair.first << " " << sessionPair.second << "\n";
        }
    }

    return oss.str();
}

std::string NoteWriter::serializeJson(
    const std::string& commit_sha,
    const std::map<std::string, Session>& sessions
) {
    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"schema\": \"ghost/1.0.0\",\n";
    oss << "  \"commit\": \"" << commit_sha << "\",\n";
    oss << "  \"sessions\": {\n";

    bool firstSession = true;
    for (const auto& pair : sessions) {
        if (!firstSession) {
            oss << ",\n";
        }
        firstSession = false;

        oss << "    \"" << escapeJson(pair.first) << "\": {\n";
        oss << "      \"session_id\": \"" << escapeJson(pair.second.session_id) << "\",\n";
        oss << "      \"agent\": \"" << escapeJson(pair.second.agent) << "\",\n";
        oss << "      \"model\": \"" << escapeJson(pair.second.model) << "\",\n";
        oss << "      \"author\": \"" << escapeJson(pair.second.author) << "\",\n";
        oss << "      \"ts_start\": " << pair.second.ts_start << ",\n";
        oss << "      \"ts_end\": " << pair.second.ts_end << ",\n";
        oss << "      \"additions\": " << pair.second.additions << ",\n";
        oss << "      \"deletions\": " << pair.second.deletions << "\n";
        oss << "    }";
    }

    oss << "\n  }\n";
    oss << "}\n";

    return oss.str();
}

std::string NoteWriter::write(
    const std::vector<AuthorshipEntry>& entries,
    const std::map<std::string, Session>& sessions,
    const std::string& commit_sha
) {
    std::string top = serializeTop(entries);
    std::string jsonSection = serializeJson(commit_sha, sessions);

    return top + "---\n" + jsonSection;
}

}
}
