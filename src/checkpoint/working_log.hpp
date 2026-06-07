#ifndef GHOST_CHECKPOINT_WORKING_LOG_HPP
#define GHOST_CHECKPOINT_WORKING_LOG_HPP

#include <string>

namespace ghost {
namespace checkpoint {

class WorkingLog {
public:
    static std::string getGhostDir(const std::string& repoRoot);
    static void ensureGhostDir(const std::string& repoRoot);
    static void clearSnapshot(const std::string& repoRoot);
};

}
}

#endif
