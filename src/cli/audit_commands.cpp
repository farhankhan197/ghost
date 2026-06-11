#include "audit_commands.hpp"
#include "exit_codes.hpp"
#include "audit/auditor.hpp"
#include "config/ghost_config.hpp"
#include "git/command.hpp"
#include "git/ref.hpp"
#include "git/repo.hpp"
#include "output/interactive.hpp"
#include "output/layout.hpp"
#include "output/report.hpp"
#include "output/style.hpp"
#include "output/ux.hpp"

#include <algorithm>
#include <iostream>
#include <map>
#include <sstream>
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

static std::string shortSourceForFile(const audit::FileAttribution& file) {
    std::map<std::string, int> counts;
    for (const auto& line : file.lines) {
        if (!line.is_ai) continue;
        std::string agent = line.agent.empty() ? "unknown" : line.agent;
        std::string model = line.model.empty() ? "unknown" : line.model;
        counts[agent + "/" + model]++;
    }
    int best = 0;
    std::string source = "unknown";
    for (const auto& [entity, lines] : counts) {
        if (lines > best) {
            best = lines;
            source = entity;
        }
    }
    return source;
}

static std::vector<audit::FileAttribution> finalDiffFiles(const audit::AuditSummary& summary, bool aiOnly) {
    std::vector<audit::FileAttribution> files;
    for (const auto& commit : summary.commits) {
        for (const auto& file : commit.files) {
            if (aiOnly && file.ai_lines <= 0) continue;
            files.push_back(file);
        }
    }
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        int ap = output::Ux::percent(a.ai_lines, a.total_lines);
        int bp = output::Ux::percent(b.ai_lines, b.total_lines);
        if ((a.ai_lines > 0) != (b.ai_lines > 0)) return a.ai_lines > 0;
        if (ap != bp) return ap > bp;
        return a.file_path < b.file_path;
    });
    return files;
}

static std::string renderFinalDiffFileTable(const audit::AuditSummary& summary, bool aiOnly) {
    using namespace ghost::output;
    std::ostringstream out;
    auto files = finalDiffFiles(summary, aiOnly);
    if (files.empty()) {
        out << "  " << Style::dim(aiOnly
            ? "No AI-attributed lines in the final PR diff."
            : "No final-diff files to show.") << "\n";
        return out.str();
    }

    size_t width = Layout::contentWidth();
    bool stacked = width < 92;
    if (!stacked) {
        size_t fileCol = width >= 110 ? 46 : 38;
        size_t linesCol = 10;
        size_t shareCol = 7;
        size_t sourceCol = width > fileCol + linesCol + shareCol + 10
            ? width - fileCol - linesCol - shareCol - 8
            : 24;
        out << "  " << Layout::fitCell(Style::dim("File"), fileCol)
            << Layout::fitCell(Style::dim("AI Lines"), linesCol)
            << Layout::fitCell(Style::dim("Share"), shareCol)
            << Style::dim("Source") << "\n";
        for (const auto& file : files) {
            std::string lines = std::to_string(file.ai_lines) + "/" + std::to_string(file.total_lines);
            std::string share = std::to_string(Ux::percent(file.ai_lines, file.total_lines)) + "%";
            out << "  " << Layout::fitCell(Style::blue(file.file_path), fileCol)
                << Layout::fitCell(Style::glow(lines), linesCol)
                << Layout::fitCell(Style::violet(share), shareCol)
                << Layout::ellipsizeMiddle(Style::glow(shortSourceForFile(file)), sourceCol) << "\n";
        }
    } else {
        size_t bodyWidth = width > 6 ? width - 6 : width;
        for (const auto& file : files) {
            std::string detail = std::to_string(file.ai_lines) + "/" + std::to_string(file.total_lines) +
                " AI lines · " + std::to_string(Ux::percent(file.ai_lines, file.total_lines)) +
                "% · " + shortSourceForFile(file);
            out << "  " << Style::blue(Layout::ellipsizeMiddle(file.file_path, bodyWidth)) << "\n";
            out << "    " << Style::dim(Layout::ellipsizeMiddle(detail, bodyWidth)) << "\n";
        }
    }
    return out.str();
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

    int pct = Ux::percent(report.summary.ai_lines, report.summary.total_lines);
    std::string threshold = Ux::thresholdText(report.policy);

    std::cout << Style::header("verify-pr");
    std::cout << "  " << Ux::verdict(report.policy)
              << Style::dim("  ") << Style::violet(std::to_string(pct) + "% AI")
              << Style::dim("  ") << Style::glow(std::to_string(report.summary.ai_lines) + "/" +
                                                  std::to_string(report.summary.total_lines) + " final-diff lines");
    if (!threshold.empty()) {
        std::cout << Style::dim("  ") << Style::muted(threshold);
    }
    std::cout << "\n";
    std::cout << "  " << Style::dim("base ") << Style::violet(base)
              << Style::dim(" · policy ") << Style::violet(configRef + ":ghost.yml")
              << Style::dim(" · range ") << Style::violet(range) << "\n\n";

    std::cout << "  " << Style::bold(Style::violet("Policy")) << "\n";
    std::cout << output::Layout::keyValue("owner policy", Style::muted(Ux::policySummary(cfg)));
    std::cout << output::Layout::keyValue("scope", Style::muted(finalDiffMode ? "final PR diff" : "commit history"));
    if (finalDiffMode) {
        std::cout << output::Layout::keyValue("history", Style::muted(cfg.history_policy.empty() ? "warn" : cfg.history_policy));
    }
    std::cout << "\n";

    std::cout << "  " << Style::bold(Style::violet("Trust")) << "\n";
    std::cout << output::Layout::keyValue("notes", Style::muted(Ux::trustSummary(attemptedFetch, noFetch, remote)));
    std::cout << "\n";

    if (report.policy.blocked) {
        std::cout << "  " << Style::bold(Style::violet("Why blocked")) << "\n";
        std::cout << "  " << Style::dim(report.policy.message.empty()
            ? "The final PR diff does not satisfy the owner policy."
            : report.policy.message) << "\n\n";

        std::cout << "  " << Style::bold(Style::violet("Files Causing The Block")) << "\n\n";
        std::cout << renderFinalDiffFileTable(report.summary, true) << "\n";

        std::cout << Ux::nextBlock({
            "ghost status",
            "git add <files> && ghost check",
            "ghost verify-pr --base " + base
        });
        std::cout << "  " << Style::warning("Do not weaken ghost.yml in this PR; base policy is used.") << "\n\n";
    } else {
        bool hasAiFiles = false;
        for (const auto& file : finalDiffFiles(report.summary, true)) {
            if (file.ai_lines > 0) {
                hasAiFiles = true;
                break;
            }
        }
        if (hasAiFiles || verbose) {
            std::cout << "  " << Style::bold(Style::violet("Files")) << "\n\n";
            std::cout << renderFinalDiffFileTable(report.summary, true) << "\n";
        }
        if (!report.policy.message.empty()) {
            std::cout << "  " << Style::dim(report.policy.message) << "\n\n";
        }
    }

    return report.policy.blocked ? kExitBlocked : kExitOk;
}

}
}
