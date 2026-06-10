#ifndef GHOST_HOOKS_AGENT_HOOKS_HPP
#define GHOST_HOOKS_AGENT_HOOKS_HPP

#include <string>
#include <vector>

namespace ghost {
namespace hooks {

class AgentHooks {
public:
    static bool installForAgent(const std::string& repoRoot, const std::string& agent, bool global);
    static bool uninstallForAgent(const std::string& repoRoot, const std::string& agent, bool global);
    static bool installAll(const std::string& repoRoot, bool global);
    static bool uninstallAll(const std::string& repoRoot, bool global);
    static std::vector<std::string> knownAgents();
    static std::vector<std::string> defaultCaptureAgents();
    static std::string displayName(const std::string& agent);
};

}
}

#endif
