#include "working_log.hpp"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <system_error>

namespace fs = std::filesystem;

namespace ghost {
namespace checkpoint {

std::string WorkingLog::getGhostDir(const std::string& repoRoot) {
    return (fs::path(repoRoot) / ".git" / "ghost").string();
}

void WorkingLog::ensureGhostDir(const std::string& repoRoot) {
    std::string ghostDir = getGhostDir(repoRoot);
    std::error_code ec;
    fs::create_directories(fs::path(ghostDir) / "snapshot", ec);
    fs::create_directories(fs::path(ghostDir) / "sessions", ec);
}

void WorkingLog::savePreState(const std::string& repoRoot, const std::string& agent, time_t ts, const std::vector<std::string>& files) {
    ensureGhostDir(repoRoot);

    std::string path = (fs::path(getGhostDir(repoRoot)) / "working.log").string();
    std::ofstream file(path);

    file << "{\"agent\":\"" << agent << "\",\"ts_start\":" << ts << ",\"files\":[";
    for (size_t i = 0; i < files.size(); ++i) {
        if (i > 0) file << ",";
        file << "\"" << files[i] << "\"";
    }
    file << "]}";
}

static std::string extractString(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":\"";
    size_t start = json.find(search);
    if (start == std::string::npos) return "";
    start += search.length();
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

static std::vector<std::string> extractStringArray(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":[";
    size_t start = json.find(search);
    if (start == std::string::npos) return {};
    start += search.length();
    size_t end = json.find("]", start);
    if (end == std::string::npos) return {};
    std::string arrStr = json.substr(start, end - start);

    std::vector<std::string> result;
    size_t pos = 0;
    while ((pos = arrStr.find("\"", pos)) != std::string::npos) {
        size_t strEnd = arrStr.find("\"", pos + 1);
        if (strEnd == std::string::npos) break;
        result.push_back(arrStr.substr(pos + 1, strEnd - pos - 1));
        pos = strEnd + 1;
    }
    return result;
}

PreState WorkingLog::loadPreState(const std::string& repoRoot) {
    PreState result;
    result.valid = false;

    std::string path = (fs::path(getGhostDir(repoRoot)) / "working.log").string();
    std::ifstream file(path);
    if (!file.is_open()) return result;

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (content.empty()) return result;

    result.agent = extractString(content, "agent");
    result.ts_start = static_cast<time_t>(extractNumber(content, "ts_start"));
    result.files = extractStringArray(content, "files");
    result.valid = true;
    return result;
}

void WorkingLog::clearPreState(const std::string& repoRoot) {
    std::string ghostDir = getGhostDir(repoRoot);
    std::error_code ec;

    fs::remove(fs::path(ghostDir) / "working.log", ec);

    fs::path snapshotDir = fs::path(ghostDir) / "snapshot";
    if (fs::exists(snapshotDir, ec)) {
        for (const auto& entry : fs::directory_iterator(snapshotDir, ec)) {
            fs::remove_all(entry.path(), ec);
        }
    }
}

void WorkingLog::saveSession(const std::string& repoRoot, const std::string& sessionId, const std::string& json) {
    ensureGhostDir(repoRoot);
    std::string path = (fs::path(getGhostDir(repoRoot)) / "sessions" / (sessionId + ".json")).string();
    std::ofstream file(path);
    file << json;
}

std::vector<std::string> WorkingLog::listSessions(const std::string& repoRoot) {
    std::vector<std::string> result;
    std::string sessionsDir = (fs::path(getGhostDir(repoRoot)) / "sessions").string();
    std::error_code ec;

    if (!fs::exists(sessionsDir, ec)) return result;

    for (const auto& entry : fs::directory_iterator(sessionsDir, ec)) {
        if (entry.is_regular_file(ec) && entry.path().extension() == ".json") {
            result.push_back(entry.path().filename().string());
        }
    }

    std::sort(result.begin(), result.end());
    return result;
}

}
}
