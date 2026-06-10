#ifndef GHOST_CLI_GOVERNANCE_COMMANDS_HPP
#define GHOST_CLI_GOVERNANCE_COMMANDS_HPP

#include <string>

namespace ghost {
namespace cli {

void printSuggestion(const std::string& unknown);
int init(int argc, char* argv[], bool verbose);
int config(int argc, char* argv[]);
int policy(int argc, char* argv[]);
int banish(int argc, char* argv[]);

}
}

#endif
