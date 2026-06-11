#ifndef GHOST_AUDIT_POLICY_HPP
#define GHOST_AUDIT_POLICY_HPP

#include <string>
#include "aggregator.hpp"
#include "../config/ghost_config.hpp"

namespace ghost {
namespace audit {

struct PolicyResult {
    bool passed;
    bool blocked;
    bool threshold_blocked;
    std::string message;
    int threshold = -1;
    std::string action;
};

class Policy {
public:
    static PolicyResult enforce(
        const AuditSummary& summary,
        const config::GhostConfig& config,
        int thresholdOverride
    );
};

}
}

#endif
