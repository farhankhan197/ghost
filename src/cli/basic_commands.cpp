#include "basic_commands.hpp"
#include "commands.hpp"
#include "exit_codes.hpp"
#include "output/style.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <string>
#include <vector>

namespace ghost {
namespace cli {

static std::string lowerString(const std::string& value) {
    std::string out = value;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

static void logVerbose(bool verbose, const std::string& msg) {
    if (verbose) {
        std::cerr << output::Style::dim("[verbose] " + msg) << "\n";
    }
}

int explain(int argc, char* argv[]) {
    using namespace ghost::output;

    std::string topic;
    if (argc > 2 && std::string(argv[2])[0] != '-') {
        topic = lowerString(argv[2]);
    }

    auto printTopic = [&](const std::string& name,
                          const std::string& stage,
                          const std::vector<std::string>& reads,
                          const std::vector<std::string>& doesNotRead,
                          const std::string& enforces,
                          const std::string& next) {
        std::cout << Style::header("Explain: " + name);
        std::cout << "  " << Style::label("stage") << "       " << Style::violet(stage) << "\n\n";

        std::cout << "  " << Style::subHeader("Reads");
        for (const auto& item : reads) {
            std::cout << "    " << Style::dim("- ") << item << "\n";
        }
        std::cout << "\n";

        if (!doesNotRead.empty()) {
            std::cout << "  " << Style::subHeader("Does Not Read");
            for (const auto& item : doesNotRead) {
                std::cout << "    " << Style::dim("- ") << item << "\n";
            }
            std::cout << "\n";
        }

        std::cout << "  " << Style::subHeader("Policy");
        std::cout << "    " << enforces << "\n\n";

        std::cout << "  " << Style::subHeader("Next");
        std::cout << "    " << next << "\n\n";
    };

    if (topic.empty()) {
        std::cout << Style::header("Explain");
        std::cout << "  " << Style::dim("Choose a command to explain:\n\n");
        std::cout << "    ghost explain init\n";
        std::cout << "    ghost explain status\n";
        std::cout << "    ghost explain check\n";
        std::cout << "    ghost explain audit\n";
        std::cout << "    ghost explain verify-pr\n";
        std::cout << "    ghost explain policy\n\n";
        return kExitOk;
    }

    if (topic == "init") {
        printTopic(
            "init",
            "setup",
            {"git repository root", "current git user.email", "existing ghost.yml when present", "detected local project directories for ignore defaults"},
            {"committed attribution notes", "pull request state"},
            "Creates maintainer policy with --owner, or installs local compliance without changing policy with --contributor.",
            "Maintainers should run 'ghost init --owner'; contributors should run 'ghost init --contributor'."
        );
        return kExitOk;
    }

    if (topic == "status") {
        printTopic(
            "status",
            "current working state",
            {"ghost.yml in the current repo", "local git hooks", "staged and unstaged working tree counts", "uncommitted Ghost agent sessions under .git/ghost", "HEAD ghost and ghost-verified notes"},
            {"full committed history", "pull request base-branch policy", "unstaged file attribution in ghost check"},
            "Does not enforce policy; it explains what exists now.",
            "Use 'ghost check' after git add, and 'ghost audit' after commit."
        );
        return kExitOk;
    }

    if (topic == "check") {
        printTopic(
            "check",
            "pre-commit preview",
            {"staged diff only", "uncommitted Ghost sessions", "open checkpoint state", "HEAD notes for existing line attribution", "current repo ghost.yml"},
            {"unstaged changes", "future commit notes", "pull request base-branch policy unless --config-ref is used"},
            "Predicts whether staged changes would exceed the local policy threshold.",
            "Run 'git add <files>' first, then 'ghost check', then commit."
        );
        return kExitOk;
    }

    if (topic == "audit") {
        printTopic(
            "audit",
            "committed codebase attribution",
            {"HEAD codebase attribution by default", "committed Git history when --range or --all is passed", "refs/notes/ghost", "refs/notes/ghost-verified", "refs/notes/ai when git-ai fallback is enabled", "ghost.yml, or base-branch ghost.yml when --config-ref is passed"},
            {"uncommitted sessions", "unstaged working tree changes", "staged changes that have not been committed"},
            "Shows the final policy result for committed code at HEAD and lists AI-touched files in one table.",
            "Use 'ghost verify-pr' before pushing. Use 'ghost audit --range BASE..HEAD --config-ref origin/main' only when you want historical commit context."
        );
        return kExitOk;
    }

    if (topic == "verify-pr" || topic == "verify") {
        printTopic(
            "verify-pr",
            "local PR simulation",
            {"the final diff for the selected PR range, defaulting to origin/main..HEAD", "base-branch ghost.yml, defaulting to origin/main:ghost.yml", "Ghost notes for commits that authored surviving final-diff lines", "historical commits as warnings unless enforcement.history is block"},
            {"GitHub review approvals", "CODEOWNERS approval state", "uncommitted local changes"},
            "Enforces the final PR diff by default; intermediate commit history is context unless configured to block.",
            "Run before pushing: 'ghost verify-pr origin/main..HEAD'."
        );
        return kExitOk;
    }

    if (topic == "policy") {
        printTopic(
            "policy",
            "owner controls",
            {"ghost.yml owner and owners allowlist", "policy mode", "locked flag", "threshold and verification policy", "banished ignore paths"},
            {"committed attribution percentages", "PR review approvals"},
            "Shows who can change protected policy and whether policy is locked.",
            "Owners can use 'ghost policy set mode <mode>', 'ghost policy lock', or 'ghost policy unlock --force'."
        );
        return kExitOk;
    }

    std::cerr << Style::error("Unknown explain topic: " + topic + "\n")
              << Style::dim("  Try: init, status, check, audit, verify-pr, policy\n");
    return kExitError;
}

int completion(int argc, char* argv[], bool verbose) {
    if (argc < 3) {
        CommandRegistry::printHelp("completion");
        return kExitError;
    }
    std::string shell = argv[2];
    logVerbose(verbose, "generating completions for: " + shell);

    if (shell == "bash") {
        std::cout << "_ghost_completion() {\n";
        std::cout << "  local cur=\"${COMP_WORDS[COMP_CWORD]}\"\n";
        std::cout << "  local cmds=\"";
        auto cmds = CommandRegistry::getAllCommands();
        for (size_t i = 0; i < cmds.size(); ++i) {
            if (i > 0) std::cout << " ";
            std::cout << cmds[i];
        }
        std::cout << "\"\n";
        std::cout << "  COMPREPLY=( $(compgen -W \"$cmds\" -- $cur) )\n";
        std::cout << "}\n";
        std::cout << "complete -F _ghost_completion ghost\n";
    } else if (shell == "zsh") {
        std::cout << "#compdef ghost\n";
        std::cout << "_ghost() {\n";
        std::cout << "  local -a cmds=(";
        auto cmds = CommandRegistry::getAllCommands();
        for (size_t i = 0; i < cmds.size(); ++i) {
            if (i > 0) std::cout << " ";
            std::cout << "\"" << cmds[i] << "\"";
        }
        std::cout << ")\n";
        std::cout << "  _describe 'ghost commands' cmds\n";
        std::cout << "}\n";
        std::cout << "compdef _ghost ghost\n";
    } else if (shell == "fish") {
        auto cmds = CommandRegistry::getAllCommands();
        for (const auto& cmd : cmds) {
            std::cout << "complete -c ghost -f -a '" << cmd << "'\n";
        }
    } else {
        std::cerr << output::Style::error("Unsupported shell: " + shell) << "\n";
        std::cerr << "Supported: bash, zsh, fish\n";
        return kExitError;
    }
    return kExitOk;
}

}
}
