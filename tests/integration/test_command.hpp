#ifndef GHOST_TEST_INTEGRATION_COMMAND_HPP
#define GHOST_TEST_INTEGRATION_COMMAND_HPP

#include "util/process.hpp"

#include <string>
#include <vector>

namespace ghost {
namespace test {

inline util::Process::Result run(
    const std::string& cwd,
    const std::string& executable,
    const std::vector<std::string>& args = {},
    const std::string& stdinText = ""
) {
    util::Process::Command command;
    command.executable = executable;
    command.args = args;
    command.cwd = cwd;
    command.stdinText = stdinText;
    command.mergeStderr = true;
    return util::Process::capture(command);
}

inline util::Process::Result git(const std::string& cwd, const std::vector<std::string>& args) {
    return run(cwd, "git", args);
}

inline std::string output(const util::Process::Result& result) {
    return result.stdoutText + result.stderrText;
}

inline std::string trimNewlines(std::string value) {
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
        value.pop_back();
    }
    return value;
}

}
}

#endif
