#ifndef GHOST_AUDIT_AGGREGATOR_HPP
#define GHOST_AUDIT_AGGREGATOR_HPP

#include <string>
#include <vector>
#include "blame_overlay.hpp"

namespace ghost {
namespace audit {

struct CommitSummary {
    std::string commit_sha;
    std::string author;
    int total_lines;
    int ai_lines;
    bool has_ghost_note;
    bool has_verified_note;
    std::vector<FileAttribution> files;
};

struct AuditSummary {
    std::vector<CommitSummary> commits;
    int total_lines;
    int ai_lines;
};

class Aggregator {
public:
    static AuditSummary aggregate(
        const std::vector<CommitSummary>& commits
    );
};

}
}

#endif
