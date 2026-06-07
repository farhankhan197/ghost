#ifndef GHOST_GIT_DIFF_HPP
#define GHOST_GIT_DIFF_HPP

#include <string>
#include <vector>
#include <map>
#include "line_range.hpp"

namespace ghost {
namespace git {

struct DiffFile {
    std::string path;
    int additions;
    int deletions;
};

struct DiffRanges {
    std::map<std::string, note::LineRangeSet> added;
    std::map<std::string, note::LineRangeSet> deleted;
    std::map<std::string, std::string> renames;
};

class Diff {
public:
    static std::vector<DiffFile> getChangedFiles(const std::string& range);
    static DiffRanges getChangedRanges(const std::string& repoRoot, const std::string& range);
    static DiffRanges getCommitRanges(const std::string& repoRoot, const std::string& commitSha);
};

}
}
#endif
