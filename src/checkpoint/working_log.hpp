#ifndef GHOST_CHECKPOINT_WORKING_LOG_HPP
#define GHOST_CHECKPOINT_WORKING_LOG_HPP

#include <string>
#include <vector>
#include <ctime>

namespace ghost {
namespace checkpoint {

struct PreState {
    std::string agent;
    time_t ts_start;
    std::vector<std::string> files;
    bool valid;
};

class WorkingLog {
public:
    static std::string getGhostDir(const std::string& repoRoot);
    static void ensureGhostDir(const std::string& repoRoot);
    static void savePreState(const std::string& repoRoot, const std::string& agent, time_t ts, const std::vector<std::string>& files);
    static PreState loadPreState(const std::string& repoRoot);
    static void clearPreState(const std::string& repoRoot);
    static void saveSession(const std::string& repoRoot, const std::string& sessionId, const std::string& json);
    static std::vector<std::string> listSessions(const std::string& repoRoot);
};

}
}

#endif
