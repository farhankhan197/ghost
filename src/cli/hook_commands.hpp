#ifndef GHOST_CLI_HOOK_COMMANDS_HPP
#define GHOST_CLI_HOOK_COMMANDS_HPP

namespace ghost {
namespace cli {

int installHooks(int argc, char* argv[], bool verbose);
int uninstallHooks(int argc, char* argv[], bool verbose);

}
}

#endif
