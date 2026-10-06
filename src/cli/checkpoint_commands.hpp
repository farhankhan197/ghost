#ifndef GHOST_CLI_CHECKPOINT_COMMANDS_HPP
#define GHOST_CLI_CHECKPOINT_COMMANDS_HPP

namespace ghost {
namespace cli {

// Agent-hook capture commands, invoked as `ghost pre|post|reset`.
// These run the checkpoint runner (snapshot/session capture) that
// used to live in the separate ghost-checkpoint binary.
int pre(int argc, char* argv[], bool verbose);
int post(int argc, char* argv[], bool verbose);
int checkpointReset(int argc, char* argv[], bool verbose);

}
}

#endif
