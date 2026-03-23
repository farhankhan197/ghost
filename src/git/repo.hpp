#ifndef GHOST_GIT_REPO_HPP
#define GHOST_GIT_REPO_HPP

#include <string>

namespace ghost {
namespace git {

class Repo {
public:
    static std::string getRoot();
    static std::string getHead();
    static bool isRepo();
};

}
}
#endif