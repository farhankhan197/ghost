#include "hook_commands.hpp"
#include "exit_codes.hpp"
#include "git/repo.hpp"
#include "hooks/agent_hooks.hpp"
#include "hooks/installer.hpp"
#include "output/style.hpp"

#include <iostream>
#include <string>

namespace ghost {
namespace cli {

static void logVerbose(bool verbose, const std::string& msg) {
    if (verbose) {
        std::cerr << output::Style::dim("[verbose] " + msg) << "\n";
    }
}

static std::string getAgentArg(int argc, char* argv[]) {
    for (int i = 2; i < argc - 1; ++i) {
        if (std::string(argv[i]) == "--agent" && i + 1 < argc) {
            return argv[i + 1];
        }
    }
    return "";
}

static bool hasFlag(int argc, char* argv[], const std::string& flag) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == flag) return true;
    }
    return false;
}

int uninstall(int argc, char* argv[]) {
    bool global = hasFlag(argc, argv, "--global") || hasFlag(argc, argv, "-g");
    if (global) {
        return hooks::Installer::uninstallGlobal();
    }
    std::string repoRoot = git::Repo::getRoot();
    if (repoRoot.empty()) {
        std::cerr << output::Style::error("Not in a git repository") << "\n";
        return kExitNotInRepo;
    }
    return hooks::Installer::uninstallRepo(repoRoot);
}

int installHooks(int argc, char* argv[], bool verbose) {
    std::string specificAgent = getAgentArg(argc, argv);

    logVerbose(verbose, "install global agent hooks, agent=" + specificAgent);
    hooks::Installer::installBin();
    if (!specificAgent.empty()) {
        if (!hooks::AgentHooks::installForAgent("", specificAgent, true)) return kExitError;
    } else {
        if (!hooks::AgentHooks::installAll("", true)) return kExitError;
    }
    return kExitOk;
}

int uninstallHooks(int argc, char* argv[], bool verbose) {
    std::string specificAgent = getAgentArg(argc, argv);

    logVerbose(verbose, "uninstall global agent hooks, agent=" + specificAgent);
    if (!specificAgent.empty()) {
        hooks::AgentHooks::uninstallForAgent("", specificAgent, true);
    } else {
        hooks::AgentHooks::uninstallAll("", true);
    }
    return kExitOk;
}

}
}
