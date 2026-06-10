#include "doctor_command.hpp"
#include "exit_codes.hpp"
#include "config/ghost_config.hpp"
#include "git/repo.hpp"
#include "hooks/agent_detector.hpp"
#include "hooks/agent_hooks.hpp"
#include "hooks/installer.hpp"
#include "output/style.hpp"
#include "util/process.hpp"

#include <filesystem>
#include <iostream>
#include <string>

namespace ghost {
namespace cli {
namespace {

bool hasFlag(int argc, char* argv[], const std::string& flag) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == flag) return true;
    }
    return false;
}

bool fileExists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

}

int doctor(int argc, char* argv[]) {
    using namespace ghost::output;

    std::cout << Style::header("Ghost Doctor");

    bool allOk = true;
    bool autoFix = hasFlag(argc, argv, "--fix") || hasFlag(argc, argv, "-f");

    std::string repoRoot = git::Repo::getRoot();
    if (repoRoot.empty()) {
        std::cout << "  " << Style::error("✗ Not in a git repository") << "\n";
        return kExitError;
    }
    std::cout << "  " << Style::success("✓ Git repository") << " " << Style::dim(repoRoot) << "\n";

    {
        std::string ghostPath = util::Process::capture("which ghost 2>/dev/null || where ghost 2>nul");
        if (ghostPath.empty() || ghostPath.find("not found") != std::string::npos) {
            std::cout << "  " << Style::warning("⚠ Ghost not in PATH") << "\n";
            std::cout << "    " << Style::dim("Run 'ghost init' or add ~/.ghost/bin to PATH") << "\n";
            allOk = false;
        } else {
            std::cout << "  " << Style::success("✓ Ghost in PATH") << " " << Style::dim(ghostPath) << "\n";
        }
    }

    std::string ymlPath = repoRoot + "/ghost.yml";
    bool ymlExists = fileExists(ymlPath);
    if (!ymlExists) {
        std::cout << "  " << Style::warning("⚠ ghost.yml not found") << "\n";
        if (autoFix) {
            config::GhostConfigReader::save(repoRoot, "threshold", "80");
            std::cout << "    " << Style::success("Fixed: created ghost.yml with defaults") << "\n";
            ymlExists = true;
        } else {
            std::cout << "    " << Style::dim("Run 'ghost init' to create one") << "\n";
            allOk = false;
        }
    } else {
        auto cfg = config::GhostConfigReader::load(repoRoot);
        std::cout << "  " << Style::success("✓ ghost.yml") << " "
                  << Style::dim("threshold=" + std::to_string(cfg.threshold) +
                                  ", required=" + (cfg.required ? "true" : "false")) << "\n";
    }

    std::string postCommitHook = repoRoot + "/.git/hooks/post-commit";
    std::string prePushHook = repoRoot + "/.git/hooks/pre-push";
    bool postCommitExists = fileExists(postCommitHook);
    bool prePushExists = fileExists(prePushHook);

    if (!postCommitExists) {
        std::cout << "  " << Style::warning("⚠ post-commit hook missing") << "\n";
        if (autoFix) {
            hooks::Installer::installRepo(repoRoot);
            std::cout << "    " << Style::success("Fixed: installed hooks") << "\n";
        } else {
            allOk = false;
        }
    } else {
        std::cout << "  " << Style::success("✓ post-commit hook") << "\n";
    }

    if (!prePushExists) {
        std::cout << "  " << Style::warning("⚠ pre-push hook missing") << "\n";
        if (autoFix && !postCommitExists) {
            hooks::Installer::installRepo(repoRoot);
        } else if (!autoFix) {
            allOk = false;
        }
    } else {
        std::cout << "  " << Style::success("✓ pre-push hook") << "\n";
    }

    {
        std::string hooksToCheck[] = {"post-rewrite", "post-merge", "post-checkout", "pre-merge-commit"};
        bool anyMissing = false;
        for (const auto& hookName : hooksToCheck) {
            std::string hookPath = repoRoot + "/.git/hooks/" + hookName;
            if (!fileExists(hookPath)) {
                std::cout << "  " << Style::warning("⚠ " + hookName + " hook missing") << "\n";
                anyMissing = true;
            } else {
                std::cout << "  " << Style::success("✓ " + hookName + " hook") << "\n";
            }
        }
        if (anyMissing && autoFix) {
            hooks::Installer::installRepo(repoRoot);
            std::cout << "    " << Style::success("Fixed: installed hooks") << "\n";
        }
        if (anyMissing && !autoFix) {
            allOk = false;
        }
    }

    {
        std::string remotePush = util::Process::capture("git config --get remote.origin.push 2>/dev/null || echo ''");
        bool hasNotesRef = (remotePush.find("refs/notes/ghost") != std::string::npos);
        if (!hasNotesRef) {
            std::cout << "  " << Style::warning("⚠ git notes push not configured") << "\n";
            if (autoFix) {
                util::Process::capture("git config --add remote.origin.push \"+refs/notes/ghost:refs/notes/ghost\"");
                util::Process::capture("git config --add remote.origin.push \"+refs/notes/ghost-verified:refs/notes/ghost-verified\"");
                std::cout << "    " << Style::success("Fixed: configured notes push") << "\n";
            } else {
                std::cout << "    " << Style::dim("Run 'git config --add remote.origin.push +refs/notes/ghost:refs/notes/ghost'") << "\n";
                allOk = false;
            }
        } else {
            std::cout << "  " << Style::success("✓ git notes push configured") << "\n";
        }
    }

    auto detected = hooks::AgentDetector::detectInstalled();
    if (detected.empty()) {
        std::cout << "  " << Style::dim("  No AI agents detected") << "\n";
    } else {
        for (const auto& agent : detected) {
            std::string agentDir = hooks::AgentDetector::getGlobalConfigDir(agent);
            bool hasHook = !agentDir.empty() && fileExists(agentDir);
            if (hasHook) {
                std::cout << "  " << Style::success("✓ " + agent + " global hook") << "\n";
            } else {
                std::cout << "  " << Style::warning("⚠ " + agent + " detected but global hook not installed") << "\n";
                if (autoFix) {
                    hooks::Installer::installBin();
                    if (hooks::AgentHooks::installForAgent(repoRoot, agent, true)) {
                        std::cout << "    " << Style::success("Fixed: installed " + agent + " global hook") << "\n";
                    }
                } else {
                    allOk = false;
                }
            }
        }
    }

    std::cout << "\n";
    if (allOk) {
        std::cout << "  " << Style::success("All checks passed") << "\n\n";
    } else {
        std::cout << Style::warning("  Some issues found. Run 'ghost doctor --fix' to auto-fix.") << "\n\n";
    }
    return allOk ? kExitOk : kExitError;
}

}
}
