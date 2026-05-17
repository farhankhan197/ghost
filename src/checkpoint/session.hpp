#ifndef GHOST_CHECKPOINT_SESSION_HPP
#define GHOST_CHECKPOINT_SESSION_HPP

#include <string>
#include <vector>
#include "line_range.hpp"

namespace ghost {
namespace checkpoint {

struct FileChanges {
    std::string file_path;
    note::LineRangeSet added_ranges;
    int additions;
    int deletions;
};

struct SessionEntry {
    std::string file_path;
    std::string ranges;
};

class Session {
public:
    static std::string generateId();
    static std::string getGitAuthor(const std::string& repoRoot);
    static FileChanges computeChanges(const std::string& snapshotPath, const std::string& currentPath, const std::string& filePath);
    static void write(
        const std::string& repoRoot,
        const std::string& sessionId,
        const std::string& agent,
        const std::string& model,
        const std::string& author,
        time_t ts_start,
        time_t ts_end,
        const std::vector<SessionEntry>& entries,
        int totalAdditions,
        int totalDeletions
    );
};

}
}

#endif
