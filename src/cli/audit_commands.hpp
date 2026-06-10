#ifndef GHOST_CLI_AUDIT_COMMANDS_HPP
#define GHOST_CLI_AUDIT_COMMANDS_HPP

namespace ghost {
namespace cli {

int audit(int argc, char* argv[], bool verbose);
int verifyPr(int argc, char* argv[], bool verbose);

}
}

#endif
