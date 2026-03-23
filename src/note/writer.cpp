#include "writer.hpp"
#include <sstream>
#include <map>
#include <algorithm>
#include <cstdio>

namespace ghost {
namespace note {

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

        oss << "    \"" << pair.first << "\": {\n";
        oss << "      \"session_id\": \"" << pair.second.session_id << "\",\n";
        oss << "      \"agent\": \"" << pair.second.agent << "\",\n";
        oss << "      \"model\": \"" << pair.second.model << "\",\n";
        oss << "      \"author\": \"" << pair.second.author << "\",\n";
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