#ifndef GHOST_CONFIG_GHOST_CONFIG_HPP
#define GHOST_CONFIG_GHOST_CONFIG_HPP

#include <string>
#include <vector>

namespace ghost {
namespace config {

struct GhostConfig {
    int version;
    bool required;
    int threshold;
    std::string on_exceed;
    bool pr_comment;
    std::vector<std::string> ignore;
    std::string untagged_policy;
    std::string unverified_policy;
    bool gitai_fallback;
};

class GhostConfigReader {
public:
    static GhostConfig load(const std::string& repoRoot);
    static bool save(const std::string& repoRoot, const std::string& key, const std::string& value);
};

}
}

#endif
