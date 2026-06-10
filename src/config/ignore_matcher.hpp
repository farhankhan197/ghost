#ifndef GHOST_CONFIG_IGNORE_MATCHER_HPP
#define GHOST_CONFIG_IGNORE_MATCHER_HPP

#include <string>
#include <vector>

namespace ghost {
namespace config {

class IgnoreMatcher {
public:
    static bool matches(const std::string& filePath, const std::vector<std::string>& patterns);
};

}
}

#endif
