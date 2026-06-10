#ifndef GHOST_CLI_BASIC_COMMANDS_HPP
#define GHOST_CLI_BASIC_COMMANDS_HPP

namespace ghost {
namespace cli {

int explain(int argc, char* argv[]);
int completion(int argc, char* argv[], bool verbose);

}
}

#endif
