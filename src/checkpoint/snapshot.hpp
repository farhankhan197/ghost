#ifndef GHOST_CHECKPOINT_SNAPSHOT_HPP
#define GHOST_CHECKPOINT_SNAPSHOT_HPP

#include <string>
#include <vector>

namespace ghost {
namespace checkpoint {

class Snapshot {
public:
    static std::vector<std::string> capture(const std::string& repoRoot);
    static bool captureSingle(const std::string& repoRoot, const std::string& filePath);
};

}
}

#endif
