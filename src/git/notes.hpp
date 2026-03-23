#ifndef GHOST_GIT_NOTES_HPP
#define GHOST_GIT_NOTES_HPP

#include <string>

namespace ghost {
namespace git {

class Notes {
public:
    static std::string show(const std::string& ref, const std::string& commit_sha);
    static bool write(const std::string& ref, const std::string& commit_sha, const std::string& content);
    static bool exists(const std::string& ref, const std::string& commit_sha);
};

}
}
#endif