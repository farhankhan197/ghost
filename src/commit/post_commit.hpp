#ifndef GHOST_COMMIT_POST_COMMIT_HPP
#define GHOST_COMMIT_POST_COMMIT_HPP

#include <string>

namespace ghost {
namespace commit {

class PostCommit {
public:
    static int run(const std::string& repoRoot, const std::string& commitSha);
};

}
}

#endif
