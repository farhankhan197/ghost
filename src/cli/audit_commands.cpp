#include "audit_commands.hpp"
#include "exit_codes.hpp"
#include "audit/auditor.hpp"
#include "config/ghost_config.hpp"
#include "git/command.hpp"
#include "git/ref.hpp"
#include "git/repo.hpp"
#include "output/interactive.hpp"
#include "output/report.hpp"
#include "output/style.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace ghost {
namespace cli {

static bool hasFlag(int argc, char* argv[], const std::string& flag) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == flag) return true;
    }
    return false;
}

static std::string getArg(int argc, char* argv[], const std::string& flag) {
    for (int i = 0; i < argc - 1; ++i) {
        if (std::string(argv[i]) == flag && i + 1 < argc) {
            return argv[i + 1];
        }
    }
    return "";
}

static void logVerbose(bool verbose, const std::string& msg) {
    if (verbose) {
        std::cerr << output::Style::dim("[verbose] " + msg) << "\n";
    }
}

int audit(int argc, char* argv[], bool verbose) {
    std::string repoRoot = git::Repo::getRoot();
    if (repoRoot.empty()) {
        std::cerr << output::Style::error("Not in a git repository") << "\n";
        return kExitNotInRepo;
    }
    std::string range = getArg(argc, argv, "--range");
    bool allMode = hasFlag(argc, argv, "--all") || hasFlag(argc, argv, "-a");
    std::string thresholdStr = getArg(argc, argv, "--threshold");
    int threshold = -1;
    if (!thresholdStr.empty()) {
        try { threshold = std::stoi(thresholdStr); } catch (...) {}
    }
    std::string configRef = getArg(argc, argv, "--config-ref");
    bool jsonOutput = hasFlag(argc, argv, "--json") || hasFlag(argc, argv, "-j");

    if (!configRef.empty() && !git::Ref::isSafeConfigRef(configRef)) {
        std::cerr << output::Style::error("Invalid config ref") << "\n";
        return kExitError;
    }
    if (!range.empty() && !git::Ref::isSafeRange(range)) {
        std::cerr << output::Style::error("Invalid commit range") << "\n";
        return kExitError;
    }

    logVerbose(verbose, "audit mode: " + std::string(allMode ? "all" : (range.empty() ? "head" : "range")));
    if (!configRef.empty()) logVerbose(verbose, "config ref: " + configRef);

    if (allMode || !range.empty()) {
        output::AnimatedSpinner spinner("scanning commits", !jsonOutput);
        audit::AuditReport report;
        if (!range.empty()) {
            logVerbose(verbose, "range: " + range);
            report = audit::Auditor::run(repoRoot, range, threshold, jsonOutput, configRef);
        } else {
            std::vector<std::string> commitShas = audit::Auditor::getCommitsWithGhostNotes();
            logVerbose(verbose, "found " + std::to_string(commitShas.size()) + " commits with ghost notes");
            report = audit::Auditor::runFromList(repoRoot, commitShas, threshold, jsonOutput, configRef);
        }
        spinner.stop();
        if (jsonOutput) {
            std::cout << output::Report::formatJSON(report.summary, report.policy);
        } else {
            std::cout << output::Report::formatCLI(report.summary, report.policy, true);
        }
        return report.policy.blocked ? kExitBlocked : kExitOk;
    }

    if (argc > 2 && std::string(argv[2])[0] != '-') {
        std::string target = argv[2];
        if (!git::Ref::isSafeCommitish(target)) {
            std::cerr << output::Style::error("Invalid commit reference") << "\n";
            return kExitError;
        }
        logVerbose(verbose, "single commit audit: " + target);
        output::AnimatedSpinner spinner("scanning codebase", !jsonOutput);
        auto cbReport = audit::Auditor::runCodebaseBlame(repoRoot, target, threshold, jsonOutput, configRef);
        spinner.stop();
        if (jsonOutput) {
            std::cout << output::Report::formatCodebaseJSON(cbReport.summary, cbReport.policy);
        } else {
            output::Report::streamCodebaseCLI(cbReport.summary, cbReport.policy);
        }
        return cbReport.policy.blocked ? kExitBlocked : kExitOk;
    }

    output::AnimatedSpinner spinner("scanning codebase", !jsonOutput);
    auto cbReport = audit::Auditor::runCodebaseBlame(repoRoot, "HEAD", threshold, jsonOutput, configRef);
    spinner.stop();
    if (jsonOutput) {
        std::cout << output::Report::formatCodebaseJSON(cbReport.summary, cbReport.policy);
    } else {
        output::Report::streamCodebaseCLI(cbReport.summary, cbReport.policy);
    }
    return cbReport.policy.blocked ? kExitBlocked : kExitOk;
}

int verifyPr(int argc, char* argv[], bool verbose) {
    std::string repoRoot = git::Repo::getRoot();
    if (repoRoot.empty()) {
        std::cerr << output::Style::error("Not in a git repository") << "\n";
        return kExitNotInRepo;
    }

    std::string base = getArg(argc, argv, "--base");
    if (base.empty()) base = "origin/main";
    if (!git::Ref::isSafeConfigRef(base)) {
        std::cerr << output::Style::error("Invalid base ref") << "\n";
        return kExitError;
    }

    std::string range;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--json" || arg == "-j" || arg == "--base") {
            if (arg == "--base" && i + 1 < argc) i++;
            continue;
        }
        if (!arg.empty() && arg[0] != '-') {
            range = arg;
            break;
        }
    }
    if (range.empty()) {
        range = base + "..HEAD";
    }
    if (!git::Ref::isSafeRange(range)) {
        std::cerr << output::Style::error("Invalid commit range") << "\n";
        return kExitError;
    }

    bool jsonOutput = hasFlag(argc, argv, "--json") || hasFlag(argc, argv, "-j");
    bool noFetch = hasFlag(argc, argv, "--no-fetch");
    std::string configRef = base;

    logVerbose(verbose, "verify-pr range=" + range + " config-ref=" + configRef);

    std::string remote = "origin";
    size_t slash = base.find('/');
    if (slash != std::string::npos && slash > 0) {
        remote = base.substr(0, slash);
    }
    if (!git::Ref::isSafeToken(remote)) {
        std::cerr << output::Style::error("Invalid remote name") << "\n";
        return kExitError;
    }

    bool attemptedFetch = false;
    if (!noFetch) {
        auto remoteUrl = git::Command::run(repoRoot, {"remote", "get-url", remote}, "", true);
        bool fetchAvailable = remoteUrl.ok() && !remoteUrl.stdoutText.empty();
        if (fetchAvailable) {
            attemptedFetch = true;
            git::Command::run(repoRoot, {"fetch", remote, "refs/notes/ghost:refs/notes/ghost"}, "", true);
            git::Command::run(repoRoot, {"fetch", remote, "refs/notes/ghost-verified:refs/notes/ghost-verified"}, "", true);
            git::Command::run(repoRoot, {"fetch", remote, "refs/notes/ghost-signatures:refs/notes/ghost-signatures"}, "", true);
            git::Command::run(repoRoot, {"fetch", remote, "refs/notes/ai:refs/notes/ai"}, "", true);
        }
    }

    auto cfg = config::GhostConfigReader::loadFromRef(repoRoot, configRef);
    bool finalDiffMode = cfg.enforcement_scope.empty() || cfg.enforcement_scope == "final_diff";

    output::AnimatedSpinner spinner(finalDiffMode ? "verifying final diff policy" : "verifying PR history policy", !jsonOutput);
    auto report = finalDiffMode
        ? audit::Auditor::runFinalDiff(repoRoot, range, -1, jsonOutput, configRef)
        : audit::Auditor::run(repoRoot, range, -1, jsonOutput, configRef);
    if (finalDiffMode && cfg.history_policy != "ignore") {
        auto historyReport = audit::Auditor::run(repoRoot, range, -1, jsonOutput, configRef);
        if (historyReport.policy.blocked) {
            if (cfg.history_policy == "block") {
                report.policy.passed = false;
                report.policy.blocked = true;
                report.policy.message += "\nHistorical commit audit is configured to block: " + historyReport.policy.message;
            } else {
                report.policy.message += "\nHistory warning: " + historyReport.policy.message;
            }
        }
    }
    spinner.stop();

    if (jsonOutput) {
        std::cout << output::Report::formatJSON(report.summary, report.policy);
        return report.policy.blocked ? kExitBlocked : kExitOk;
    }

    using namespace ghost::output;

    std::cout << Style::header("Local PR Verification");
    std::cout << "  " << Style::subHeader("Policy");
    std::cout << "    " << Style::label("source") << "      " << Style::violet(configRef + ":ghost.yml") << "\n";
    std::cout << "    " << Style::label("mode") << "        " << Style::violet(cfg.mode.empty() ? "custom" : cfg.mode) << "\n";
    std::cout << "    " << Style::label("scope") << "       " << Style::violet(finalDiffMode ? "final_diff" : "commit_history") << "\n";
    std::cout << "    " << Style::label("history") << "     " << Style::violet(cfg.history_policy.empty() ? "warn" : cfg.history_policy) << "\n";
    std::cout << "    " << Style::label("threshold") << "   " << Style::violet(std::to_string(cfg.threshold) + "%") << "\n";
    std::cout << "    " << Style::label("unverified") << "  " << Style::violet(cfg.unverified_policy) << "\n\n";

    std::cout << "  " << Style::subHeader("Range");
    std::cout << "    " << Style::violet(range) << "\n\n";

    std::cout << "  " << Style::subHeader("Notes");
    if (noFetch) {
        std::cout << "    " << Style::warning("fetch skipped") << Style::dim("  --no-fetch was set") << "\n\n";
    } else if (attemptedFetch) {
        std::cout << "    " << Style::success("fetched") << Style::dim("  Ghost note refs from " + remote) << "\n\n";
    } else {
        std::cout << "    " << Style::warning("not fetched") << Style::dim("  remote " + remote + " was not available") << "\n\n";
    }

    std::cout << output::Report::formatCLI(report.summary, report.policy, true);

    if (report.policy.blocked) {
        std::cout << Style::dim("  Fix: run 'ghost init --contributor', recommit affected changes, then push notes.\n\n");
    }

    return report.policy.blocked ? kExitBlocked : kExitOk;
}

}
}
