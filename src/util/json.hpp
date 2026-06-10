#ifndef GHOST_UTIL_JSON_HPP
#define GHOST_UTIL_JSON_HPP

#include <string>

namespace ghost {
namespace util {

class Json {
public:
    static std::string escape(const std::string& value);
};

}
}

#endif
