#include "util/process.hpp"
#include <gtest/gtest.h>
#include <filesystem>

namespace {

std::string trim(std::string value) {
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
        value.pop_back();
    }
    return value;
}

}

TEST(ProcessUtil, CapturesStdoutAndQuotedArgs) {
    ghost::util::Process::Command command;
    command.executable = "git";
    command.args = {"-c", "user.name=A B \"C\"", "config", "user.name"};

    auto result = ghost::util::Process::capture(command);

    EXPECT_EQ(result.exitCode, 0);
    EXPECT_EQ(trim(result.stdoutText), "A B \"C\"");
}

TEST(ProcessUtil, SupportsCwdAndStdin) {
    ghost::util::Process::Command command;
    command.executable = "git";
    command.args = {"hash-object", "--stdin"};
    command.cwd = std::filesystem::current_path().string();
    command.stdinText = "ghost\n";

    auto result = ghost::util::Process::capture(command);

    EXPECT_EQ(result.exitCode, 0);
    EXPECT_EQ(trim(result.stdoutText), "f34fb621dea048c838ee4afdbbac8e9729b357ed");
}

TEST(ProcessUtil, ReturnsNonZeroAndStderr) {
    ghost::util::Process::Command command;
    command.executable = "git";
    command.args = {"rev-parse", "--verify", "definitely-missing-ghost-ref"};

    auto result = ghost::util::Process::capture(command);

    EXPECT_NE(result.exitCode, 0);
    EXPECT_FALSE(result.stderrText.empty());
}
