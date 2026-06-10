#ifndef GHOST_UTIL_PROCESS_HPP
#define GHOST_UTIL_PROCESS_HPP

#include <string>
#include <vector>

namespace ghost {
namespace util {

class Process {
public:
    struct Command {
        std::string executable;
        std::vector<std::string> args;
        std::string cwd;
        std::string stdinText;
        bool mergeStderr = false;
    };

    struct Result {
        int exitCode = -1;
        std::string stdoutText;
        std::string stderrText;

        bool ok() const { return exitCode == 0; }
    };

    static Result capture(const Command& command);
    static Result run(const Command& command);

    static std::string capture(const std::string& command);
};

}
}

#endif
