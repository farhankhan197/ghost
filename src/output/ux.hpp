#ifndef GHOST_OUTPUT_UX_HPP
#define GHOST_OUTPUT_UX_HPP

#include "audit/policy.hpp"
#include "config/ghost_config.hpp"

#include <string>
#include <vector>

namespace ghost {
namespace output {

class Ux {
public:
    static int percent(int part, int total);
    static std::string verdict(const audit::PolicyResult& policy);
    static std::string thresholdText(const audit::PolicyResult& policy);
    static std::string policySummary(const config::GhostConfig& config);
    static std::string trustSummary(bool notesFetched, bool fetchSkipped, const std::string& remote);
    static std::string checkRow(const std::string& state, const std::string& label, const std::string& detail = "");
    static std::string nextBlock(const std::vector<std::string>& lines);
};

}
}

#endif
