#ifndef GHOST_GIT_DIFF_HPP
#define GHOST_GIT_DIFF_HPP

#include <string>
#include <vector>

namespace ghost {
namespace git {

struct DiffFile {
    std::string path;
    int additions;
    int deletions;
};

class Diff {
public:
    static std::vector<DiffFile> getChangedFiles(const std::string& range);
};

}
}
#endif