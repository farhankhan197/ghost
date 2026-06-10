#ifndef GHOST_CLI_INSPECTION_COMMANDS_HPP
#define GHOST_CLI_INSPECTION_COMMANDS_HPP

namespace ghost {
namespace cli {

int show(int argc, char* argv[], bool verbose);
int blame(int argc, char* argv[], bool verbose);
int stats(int argc, char* argv[], bool verbose);

}
}

#endif
