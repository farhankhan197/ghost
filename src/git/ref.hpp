#ifndef GHOST_GIT_REF_HPP
#define GHOST_GIT_REF_HPP

#include <string>

namespace ghost {
namespace git {

class Ref {
public:
    static bool isSafeToken(const std::string& value);
    static bool isSafeCommitish(const std::string& value);
    static bool isSafeRange(const std::string& value);
    static bool isSafeConfigRef(const std::string& value);
    static bool isSafeNotesRef(const std::string& value);
};

}
}

#endif
