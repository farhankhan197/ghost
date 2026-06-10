#ifndef GHOST_CHECKPOINT_SESSION_JSON_HPP
#define GHOST_CHECKPOINT_SESSION_JSON_HPP

#include "session.hpp"
#include "line_range.hpp"

#include <ctime>
#include <optional>
#include <string>
#include <vector>

namespace ghost {
namespace checkpoint {

struct CapturedSession {
    int db_id = -1;
    std::string session_id;
    std::string agent;
    std::string model;
    std::string author;
    time_t ts_start = 0;
    time_t ts_end = 0;
    int additions = 0;
    int deletions = 0;
    bool committed = false;
    std::vector<SessionEntry> entries;
};

class SessionJson {
public:
    static std::string write(const CapturedSession& session);
    static std::optional<CapturedSession> parse(const std::string& jsonText);
    static std::vector<std::string> files(const std::string& jsonText, const std::string& repoRoot);
    static note::LineRangeSet rangesForFile(const std::string& jsonText, const std::string& filePath, const std::string& repoRoot);
    static std::string fingerprint(const CapturedSession& session, const std::string& repoRoot);
};

}
}

#endif
