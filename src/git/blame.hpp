#ifndef GHOST_GIT_BLAME_HPP
#define GHOST_GIT_BLAME_HPP

#include <string>
#include <map>

namespace ghost {
namespace git {

class Blame {
public:
    static std::map<int, std::string> getLineAuthorMap(const std::string& file_path);
};

}
}
#endif