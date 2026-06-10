#include "command.hpp"

#include <utility>

namespace ghost {
namespace git {

util::Process::Result Command::run(
    const std::string& repoRoot,
    std::vector<std::string> args,
    const std::string& stdinText,
    bool mergeStderr
) {
    util::Process::Command command;
    command.executable = "git";
    command.args = std::move(args);
    command.cwd = repoRoot;
    command.stdinText = stdinText;
    command.mergeStderr = mergeStderr;
    return util::Process::capture(command);
}

std::string Command::capture(
    const std::string& repoRoot,
    std::vector<std::string> args,
    const std::string& stdinText,
    bool mergeStderr
) {
    return run(repoRoot, std::move(args), stdinText, mergeStderr).stdoutText;
}

}
}
