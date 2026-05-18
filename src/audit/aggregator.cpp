#include "aggregator.hpp"

namespace ghost {
namespace audit {

AuditSummary Aggregator::aggregate(const std::vector<CommitSummary>& commits) {
    AuditSummary summary;
    summary.total_lines = 0;
    summary.ai_lines = 0;
    summary.commits = commits;

    for (const auto& commit : commits) {
        summary.total_lines += commit.total_lines;
        summary.ai_lines += commit.ai_lines;
    }

    return summary;
}

}
}
