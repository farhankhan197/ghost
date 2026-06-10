#ifndef GHOST_GIT_COMMAND_HPP
#define GHOST_GIT_COMMAND_HPP

#include "util/process.hpp"

#include <string>
#include <vector>

namespace ghost {
namespace git {

class Command {
public:
    static util::Process::Result run(
        const std::string& repoRoot,
        std::vector<std::string> args,
        const std::string& stdinText = "",
        bool mergeStderr = false
    );

    static std::string capture(
        const std::string& repoRoot,
        std::vector<std::string> args,
        const std::string& stdinText = "",
        bool mergeStderr = false
    );
};

}
}

#endif
