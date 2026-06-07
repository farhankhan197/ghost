#include "reader.hpp"
#include <sstream>
#include <algorithm>

namespace ghost {
namespace note {

static std::string unescapeJsonString(const std::string& value) {
    std::string result;
    bool escaped = false;
    for (char c : value) {
        if (escaped) {
            switch (c) {
                case 'n': result += '\n'; break;
                case 'r': result += '\r'; break;
                case 't': result += '\t'; break;
                default: result += c; break;
            }
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        result += c;
    }
    return result;
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
    std::string result;
    bool escaped = false;
    for (size_t i = start; i < json.size(); ++i) {
        char c = json[i];
        if (escaped) {
            result += '\\';
            result += c;
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '"') return unescapeJsonString(result);
        result += c;
    }
    return "";
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

NoteReader::Result NoteReader::parse(const std::string& note_content) {
    Result result;
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

    while (std::getline(topStream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) continue;

        if (line[0] != ' ') {
            currentFile = line;
        } else {
            std::string trimmed = line.substr(2);
            size_t spacePos = trimmed.find(' ');
            if (spacePos == std::string::npos) continue;

            std::string sessionId = trimmed.substr(0, spacePos);
            std::string rangesStr = trimmed.substr(spacePos + 1);

            AuthorshipEntry entry;
            entry.file_path = currentFile;
            entry.session_id = sessionId;
            if (!rangesStr.empty()) {
                try {
                    entry.ranges = LineRangeSet::parse(rangesStr);
                } catch (...) {
                    continue;
                }
            }
            result.entries.push_back(entry);
            result.entries_by_file[entry.file_path].push_back(entry);
        }
    }

    result.commit_sha = extractString(jsonSection, "commit");

    size_t sessionsStart = jsonSection.find("\"sessions\":");
    if (sessionsStart != std::string::npos) {
        size_t braceStart = jsonSection.find("{", sessionsStart + 1);
        size_t braceEnd = jsonSection.rfind("}");
        if (braceStart != std::string::npos && braceEnd != std::string::npos && braceEnd > braceStart) {
            std::string sessionsBlock = jsonSection.substr(braceStart, braceEnd - braceStart + 1);

            size_t pos = 1;
            while (pos < sessionsBlock.size()) {
                while (pos < sessionsBlock.size() && (sessionsBlock[pos] == ' ' || sessionsBlock[pos] == '\n' || sessionsBlock[pos] == '\r' || sessionsBlock[pos] == '\t' || sessionsBlock[pos] == ',')) pos++;
                if (pos >= sessionsBlock.size() || sessionsBlock[pos] == '}') break;
                if (sessionsBlock[pos] != '"') {
                    pos++;
                    continue;
                }
                size_t idEnd = pos + 1;
                bool escaped = false;
                for (; idEnd < sessionsBlock.size(); ++idEnd) {
                    if (escaped) {
                        escaped = false;
                        continue;
                    }
                    if (sessionsBlock[idEnd] == '\\') {
                        escaped = true;
                        continue;
                    }
                    if (sessionsBlock[idEnd] == '"') break;
                }
                if (idEnd == std::string::npos || idEnd >= sessionsBlock.size()) break;
                std::string sessionId = unescapeJsonString(sessionsBlock.substr(pos + 1, idEnd - pos - 1));

                size_t objStart = sessionsBlock.find("{", idEnd);
                if (objStart == std::string::npos) break;

                int braceCount = 1;
                size_t objEnd = objStart + 1;
                while (objEnd < sessionsBlock.size() && braceCount > 0) {
                    if (sessionsBlock[objEnd] == '{') braceCount++;
                    else if (sessionsBlock[objEnd] == '}') braceCount--;
                    objEnd++;
                }

                std::string sessJson = sessionsBlock.substr(objStart, objEnd - objStart);
                Session sess;
                sess.session_id = sessionId;
                sess.agent = extractString(sessJson, "agent");
                sess.model = extractString(sessJson, "model");
                sess.author = extractString(sessJson, "author");
                sess.ts_start = static_cast<time_t>(extractNumber(sessJson, "ts_start"));
                sess.ts_end = static_cast<time_t>(extractNumber(sessJson, "ts_end"));
                sess.additions = static_cast<int>(extractNumber(sessJson, "additions"));
                sess.deletions = static_cast<int>(extractNumber(sessJson, "deletions"));
                result.sessions[sessionId] = sess;

                pos = objEnd;
            }
        }
    }

    result.success = true;
    return result;
}

}
}
