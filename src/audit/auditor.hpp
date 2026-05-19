#ifndef GHOST_AUDIT_AUDITOR_HPP
#define GHOST_AUDIT_AUDITOR_HPP

#include <string>
#include <vector>
#include "blame_overlay.hpp"
#include "aggregator.hpp"
#include "policy.hpp"
#include "../config/ghost_config.hpp"

namespace ghost {
namespace audit {

struct AuditReport {
    AuditSummary summary;
    PolicyResult policy;
    bool json;
};

class Auditor {
public:
    static AuditReport run(
        const std::string& repoRoot,
        const std::string& range,
        int thresholdOverride,
        bool jsonOutput
    );

    static AuditReport runFromList(
        const std::string& repoRoot,
        const std::vector<std::string>& commitShas,
        int thresholdOverride,
        bool jsonOutput
    );

    static std::vector<std::string> getCommitsWithGhostNotes();
};

}
}

#endif
