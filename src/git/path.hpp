#ifndef GHOST_GIT_PATH_HPP
#define GHOST_GIT_PATH_HPP

#include <string>

namespace ghost {
namespace git {

struct NormalizedPath {
    std::string path;
    bool inside_repo = false;
};

class Path {
public:
    static NormalizedPath normalizeRepoPath(const std::string& path, const std::string& repoRoot);
    static std::string normalizeRepoPathOrEmpty(const std::string& path, const std::string& repoRoot);
};

}
}

#endif
