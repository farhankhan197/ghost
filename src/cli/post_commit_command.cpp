#include "post_commit_command.hpp"
#include "exit_codes.hpp"
#include "commit/post_commit.hpp"
#include "git/repo.hpp"
#include "output/style.hpp"

#include <filesystem>
#include <iostream>
#include <string>

namespace ghost {
namespace cli {

static void logVerbose(bool verbose, const std::string& msg) {
    if (verbose) {
        std::cerr << output::Style::dim("[verbose] " + msg) << "\n";
    }
}

int postCommit(int argc, char* argv[], bool verbose) {
    (void)argc;
    (void)argv;

    std::string repoRoot = git::Repo::getRoot();
    std::string commitSha = git::Repo::getHead();
    if (repoRoot.empty() || commitSha.empty()) {
        std::cerr << output::Style::error("Not in a git repository") << "\n";
        return kExitNotInRepo;
    }

    std::error_code cwdEc;
    std::filesystem::current_path(repoRoot, cwdEc);
    logVerbose(verbose, "post-commit for: " + commitSha);
    return commit::PostCommit::run(repoRoot, commitSha);
}

}
}
