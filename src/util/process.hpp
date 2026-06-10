#ifndef GHOST_UTIL_PROCESS_HPP
#define GHOST_UTIL_PROCESS_HPP

#include <string>

namespace ghost {
namespace util {

class Process {
public:
    static std::string capture(const std::string& command);
};

}
}

#endif
