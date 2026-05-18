#include "policy.hpp"
#include <sstream>

namespace ghost {
namespace audit {

PolicyResult Policy::enforce(
    const AuditSummary& summary,
    const config::GhostConfig& config,
    int thresholdOverride
) {
    PolicyResult result;
    result.passed = true;
    result.blocked = false;

    int threshold = (thresholdOverride >= 0) ? thresholdOverride : config.threshold;
    std::ostringstream msg;

    for (const auto& commit : summary.commits) {
        if (!commit.has_verified_note) {
            if (config.unverified_policy == "block") {
                msg << "Commit " << commit.commit_sha.substr(0, 8)
                    << " is missing ghost-verified note (ghost was not running).\n";
                result.passed = false;
                result.blocked = true;
            } else if (config.unverified_policy == "warn") {
                msg << "Warning: commit " << commit.commit_sha.substr(0, 8)
                    << " is missing ghost-verified note.\n";
            }
        }
    }

    double aiPct = 0.0;
    if (summary.total_lines > 0) {
        aiPct = (100.0 * summary.ai_lines) / summary.total_lines;
    }

    if (aiPct > threshold) {
        msg << "AI-authored lines: " << summary.ai_lines << "/" << summary.total_lines
            << " (" << static_cast<int>(aiPct) << "%) — exceeds threshold of " << threshold << "%";
        if (config.on_exceed == "block") {
            msg << " — BLOCKED.";
            result.blocked = true;
        } else {
            msg << " — warning only.";
        }
        result.passed = false;
    }

    if (result.passed) {
        msg << "All checks passed.";
    }

    result.message = msg.str();
    return result;
}

}
}
