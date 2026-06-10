#ifndef GHOST_CLI_EXIT_CODES_HPP
#define GHOST_CLI_EXIT_CODES_HPP

namespace ghost {
namespace cli {

constexpr int kExitOk = 0;
constexpr int kExitError = 1;
constexpr int kExitBlocked = 2;
constexpr int kExitNotInRepo = 3;

}
}

#endif
