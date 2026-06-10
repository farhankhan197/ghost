#ifndef GHOST_HOOKS_AGENT_DETECTOR_HPP
#define GHOST_HOOKS_AGENT_DETECTOR_HPP

#include <string>
#include <vector>

namespace ghost {
namespace hooks {

class AgentDetector {
public:
    static std::vector<std::string> detectInstalled();
    static bool isInstalled(const std::string& agent);
    static std::string getGlobalConfigDir(const std::string& agent);
    static std::string getLegacyRepoConfigDir(const std::string& agent, const std::string& repoRoot);
};

}
}

#endif
