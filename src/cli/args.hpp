#ifndef GHOST_CLI_ARGS_HPP
#define GHOST_CLI_ARGS_HPP

#include <string>
#include <vector>

namespace ghost {
namespace cli {

class Args {
public:
    Args(int argc, char* argv[]);

    bool hasFlag(const std::string& flag) const;
    bool hasAnyFlag(const std::vector<std::string>& flags) const;
    std::string getValue(const std::string& flag) const;
    std::vector<std::string> positional(size_t startIndex = 1) const;
    const std::vector<std::string>& all() const;

private:
    std::vector<std::string> args_;
};

}
}

#endif
