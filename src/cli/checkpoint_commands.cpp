#include "checkpoint_commands.hpp"
#include "checkpoint/runner.hpp"

#include <string>

namespace ghost {
namespace cli {

int pre(int argc, char* argv[], bool verbose) {
    (void)verbose;
    return checkpoint::run_checkpoint("pre", argc, argv);
}

int post(int argc, char* argv[], bool verbose) {
    (void)verbose;
    return checkpoint::run_checkpoint("post", argc, argv);
}

int checkpointReset(int argc, char* argv[], bool verbose) {
    (void)verbose;
    return checkpoint::run_checkpoint("reset", argc, argv);
}

}
}
