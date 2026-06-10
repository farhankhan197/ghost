#ifndef GHOST_UTIL_TEXT_HPP
#define GHOST_UTIL_TEXT_HPP

#include <optional>
#include <string>
#include <vector>

namespace ghost {
namespace util {

class Text {
public:
    static std::string trim(const std::string& value);
    static std::string lower(const std::string& value);
    static std::vector<std::string> splitLines(const std::string& value);
    static std::optional<int> parseInt(const std::string& value);
};

}
}

#endif
