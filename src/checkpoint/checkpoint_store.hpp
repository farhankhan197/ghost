#ifndef GHOST_CHECKPOINT_CHECKPOINT_STORE_HPP
#define GHOST_CHECKPOINT_CHECKPOINT_STORE_HPP

#include <string>

namespace ghost {
namespace checkpoint {

class CheckpointStore {
public:
    static std::string getGhostDir(const std::string& repoRoot);
    static void ensureGhostDir(const std::string& repoRoot);
    static void clearSnapshot(const std::string& repoRoot);
};

}
}

#endif
