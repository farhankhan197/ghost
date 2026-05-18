#include "verified_reader.hpp"
#include <sstream>

namespace ghost {
namespace note {

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

VerifiedReader::Result VerifiedReader::parse(const std::string& json_content) {
    Result result;
    result.success = false;

    if (json_content.empty()) {
        result.error = "Empty content";
        return result;
    }

    result.note.schema = extractString(json_content, "schema");
    result.note.ghost_version = extractString(json_content, "ghost_version");
    result.note.commit = extractString(json_content, "commit");
    result.note.ts = static_cast<time_t>(extractNumber(json_content, "ts"));
    result.note.author = extractString(json_content, "author");
    result.note.sessions = static_cast<int>(extractNumber(json_content, "sessions"));

    result.success = !result.note.schema.empty();
    if (!result.success) {
        result.error = "Missing schema field";
    }

    return result;
}

}
}
