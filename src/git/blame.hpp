#ifndef GHOST_GIT_BLAME_HPP
#define GHOST_GIT_BLAME_HPP

#include <string>
#include <vector>

namespace ghost {
namespace git {

struct BlameResult {
    std::vector<std::string> lines;  // lines[i] = commit SHA for line (i+1)
    bool empty() const { return lines.empty(); }
    size_t size() const { return lines.size(); }
    const std::string& operator[](size_t idx) const { return lines[idx]; }
};

class Blame {
public:
    static BlameResult getLineAuthorMap(const std::string& file_path);
    static BlameResult getLineAuthorMap(const std::string& file_path, const std::string& commit_sha);
};

}
}
#endif
