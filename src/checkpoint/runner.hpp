#ifndef GHOST_CHECKPOINT_RUNNER_HPP
#define GHOST_CHECKPOINT_RUNNER_HPP

#include <string>

namespace ghost {
namespace checkpoint {

// Run a checkpoint subcommand (pre, post, reset) as invoked via
// `ghost <command> [options]`. argc/argv include the full process
// argument list (argv[0] is the program name); flags are scanned
// position-independently, so `ghost pre --agent x` works the same
// as the old `ghost-checkpoint pre --agent x`.
int run_checkpoint(const std::string& command, int argc, char* argv[]);

} // namespace checkpoint
} // namespace ghost

#endif
