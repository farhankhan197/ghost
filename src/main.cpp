#include <iostream>
#include <string>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <memory>
#include <cstdio>
#include "git/repo.hpp"
#include "git/notes.hpp"
#include "git/blame.hpp"
#include "git/diff.hpp"
#include "note/reader.hpp"
#include "commit/post_commit.hpp"
#include "checkpoint/working_log.hpp"
#include "checkpoint/session.hpp"
#include "hooks/installer.hpp"
#include "hooks/agent_hooks.hpp"
#include "hooks/agent_detector.hpp"
#include "audit/auditor.hpp"
#include "audit/blame_overlay.hpp"
#include "audit/aggregator.hpp"
#include "audit/policy.hpp"
#include "output/report.hpp"
#include "output/style.hpp"
#include "output/interactive.hpp"
#include "config/ghost_config.hpp"
#include "cli/commands.hpp"

// Verbose logging utility
static bool g_verbose = false;
static void logVerbose(const std::string& msg) {
    if (g_verbose) {
        std::cerr << ghost::output::Style::dim("[verbose] " + msg) << "\n";
    }
}

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

// Exit codes (avoid standard macro conflicts)
static constexpr int GHOST_EXIT_OK = 0;
static constexpr int GHOST_EXIT_ERROR = 1;
static constexpr int GHOST_EXIT_BLOCKED = 2;
static constexpr int GHOST_EXIT_NOT_IN_REPO = 3;

static std::string execCommand(const std::string& cmd) {
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) return "";
    char buffer[1024];
    while (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
        result += buffer;
    }
    // Trim trailing whitespace
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r' || result.back() == ' ')) {
        result.pop_back();
    }
    return result;
}

static bool fileExists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

static void printSuggestion(const std::string& unknown) {
    auto suggestions = ghost::cli::CommandRegistry::getSuggestions(unknown);
    if (!suggestions.empty()) {
        std::cerr << "\n" << ghost::output::Style::dim("Did you mean?");
        for (const auto& s : suggestions) {
            std::cerr << "  " << ghost::output::Style::violet("ghost " + s);
        }
        std::cerr << "\n";
    }
    std::cerr << ghost::output::Style::dim("Run 'ghost help' for all available commands.\n");
}

static int handleInstall(int argc, char* argv[]) {
    logVerbose("processing install command");
    bool dryRun = hasFlag(argc, argv, "--dry-run") || hasFlag(argc, argv, "-n");
    bool global = hasFlag(argc, argv, "--global") || hasFlag(argc, argv, "-g");
    
    if (dryRun) {
        std::cout << ghost::output::Style::header("Dry Run — ghost install");
        std::cout << "Would install:\n";
        std::cout << "  - ghost binary to system PATH\n";
        std::cout << "  - post-commit hook script\n";
        std::cout << "  - pre-push hook script\n";
        std::cout << "  - git notes ref: refs/notes/ghost\n";
        if (global) std::cout << "  (global mode: git-core hooks)\n";
        else std::cout << "  (repo mode: .git/hooks/)\n";
        return GHOST_EXIT_OK;
    }
    
    if (global) {
        return ghost::hooks::Installer::installGlobal();
    }
    if (argc > 2 && std::string(argv[2]) == "-bin") {
        return ghost::hooks::Installer::installBin();
    }
    std::string repoRoot = ghost::git::Repo::getRoot();
    if (repoRoot.empty()) {
        std::cerr << ghost::output::Style::error("Not in a git repository") << "\n";
        std::cerr << ghost::output::Style::dim("Run 'ghost install --global' for system-wide installation.\n");
        return GHOST_EXIT_NOT_IN_REPO;
    }
    logVerbose("repo root: " + repoRoot);
    int binResult = ghost::hooks::Installer::installBin();
    if (binResult != GHOST_EXIT_OK) {
        std::cerr << ghost::output::Style::warning("Warning: failed to install binaries, plugin may not work") << "\n";
    }
    return ghost::hooks::Installer::installRepo(repoRoot);
}

static int handleUninstall(int argc, char* argv[]) {
    bool global = hasFlag(argc, argv, "--global") || hasFlag(argc, argv, "-g");
    if (global) {
        return ghost::hooks::Installer::uninstallGlobal();
    }
    std::string repoRoot = ghost::git::Repo::getRoot();
    if (repoRoot.empty()) {
        std::cerr << ghost::output::Style::error("Not in a git repository") << "\n";
        return GHOST_EXIT_NOT_IN_REPO;
    }
    return ghost::hooks::Installer::uninstallRepo(repoRoot);
}

static int handleShow(int argc, char* argv[]) {
    if (argc < 3 || std::string(argv[2])[0] == '-') {
        ghost::cli::CommandRegistry::printHelp("show");
        return GHOST_EXIT_ERROR;
    }
    std::string commit_sha = argv[2];
    logVerbose("showing ghost note for: " + commit_sha);
    std::string note = ghost::git::Notes::show("refs/notes/ghost", commit_sha);
    if (note.empty()) {
        std::cout << ghost::output::Style::warning("  No ghost note found for " + commit_sha) << "\n";
    } else {
        auto result = ghost::note::NoteReader::parse(note);
        if (!result.success) {
            std::cout << ghost::output::Style::error("  Failed to parse note: " + result.error) << "\n";
            std::cout << "\n" << ghost::output::Style::dim(note) << "\n";
        } else {
            using namespace ghost::output;
            std::cout << Style::header("Commit Attribution");
            std::cout << "  " << Style::label("sha") << " " << Style::violet(commit_sha) << "\n\n";

            for (const auto& entry : result.entries) {
                std::cout << "  " << Style::blue(entry.file_path) << "\n";
                auto it = result.sessions.find(entry.session_id);
                if (it != result.sessions.end()) {
                    const auto& sess = it->second;
                    std::cout << "    " << Style::muted(entry.session_id)
                              << "  " << Style::progressBar(100, 100, 5)
                              << "  " << Style::glow(sess.agent) << Style::dim("/") << Style::glow(sess.model) << "\n";
                } else {
                    std::cout << "    " << Style::muted(entry.session_id)
                              << "  " << Style::violet(entry.ranges.toString()) << "\n";
                }
            }
            std::cout << "\n";
        }
    }
    return GHOST_EXIT_OK;
}

static int handleAudit(int argc, char* argv[]) {
    std::string repoRoot = ghost::git::Repo::getRoot();
    if (repoRoot.empty()) {
        std::cerr << ghost::output::Style::error("Not in a git repository") << "\n";
        return GHOST_EXIT_NOT_IN_REPO;
    }
    std::string range = getArg(argc, argv, "--range");
    bool allMode = hasFlag(argc, argv, "--all") || hasFlag(argc, argv, "-a");
    std::string thresholdStr = getArg(argc, argv, "--threshold");
    int threshold = -1;
    if (!thresholdStr.empty()) {
        try { threshold = std::stoi(thresholdStr); } catch (...) {}
    }
    bool jsonOutput = hasFlag(argc, argv, "--json") || hasFlag(argc, argv, "-j");

    logVerbose("audit mode: " + std::string(allMode ? "all" : (range.empty() ? "head" : "range")));
    
    if (allMode || !range.empty()) {
        ghost::output::AnimatedSpinner spinner("scanning commits...");
        ghost::audit::AuditReport report;
        if (!range.empty()) {
            logVerbose("range: " + range);
            report = ghost::audit::Auditor::run(repoRoot, range, threshold, jsonOutput);
        } else {
            std::vector<std::string> commitShas = ghost::audit::Auditor::getCommitsWithGhostNotes();
            logVerbose("found " + std::to_string(commitShas.size()) + " commits with ghost notes");
            report = ghost::audit::Auditor::runFromList(repoRoot, commitShas, threshold, jsonOutput);
        }
        spinner.stop();
        if (jsonOutput) {
            std::cout << ghost::output::Report::formatJSON(report.summary, report.policy);
        } else {
            std::cout << ghost::output::Report::formatCLI(report.summary, report.policy, true);
        }
        return report.policy.blocked ? GHOST_EXIT_BLOCKED : GHOST_EXIT_OK;
    } else if (argc > 2 && std::string(argv[2])[0] != '-') {
        std::string target = argv[2];
        logVerbose("single commit audit: " + target);
        ghost::output::AnimatedSpinner spinner("scanning codebase...");
        auto cbReport = ghost::audit::Auditor::runCodebaseBlame(repoRoot, target, threshold, jsonOutput);
        spinner.stop();
        if (jsonOutput) {
            std::cout << ghost::output::Report::formatCodebaseJSON(cbReport.summary, cbReport.policy);
        } else {
            ghost::output::Report::streamCodebaseCLI(cbReport.summary, cbReport.policy);
        }
        return cbReport.policy.blocked ? GHOST_EXIT_BLOCKED : GHOST_EXIT_OK;
    } else {
        ghost::output::AnimatedSpinner spinner("scanning codebase...");
        auto cbReport = ghost::audit::Auditor::runCodebaseBlame(repoRoot, "HEAD", threshold, jsonOutput);
        spinner.stop();
        if (jsonOutput) {
            std::cout << ghost::output::Report::formatCodebaseJSON(cbReport.summary, cbReport.policy);
        } else {
            ghost::output::Report::streamCodebaseCLI(cbReport.summary, cbReport.policy);
        }
        return cbReport.policy.blocked ? GHOST_EXIT_BLOCKED : GHOST_EXIT_OK;
    }
}

static int handleBlame(int argc, char* argv[]) {
    if (argc < 3 || std::string(argv[2])[0] == '-') {
        ghost::cli::CommandRegistry::printHelp("blame");
        return GHOST_EXIT_ERROR;
    }
    std::string filePath = argv[2];
    std::string repoRoot = ghost::git::Repo::getRoot();
    if (repoRoot.empty()) {
        std::cerr << ghost::output::Style::error("Not in a git repository") << "\n";
        return GHOST_EXIT_NOT_IN_REPO;
    }
    std::string headSha = ghost::git::Repo::getHead();
    bool jsonOutput = hasFlag(argc, argv, "--json") || hasFlag(argc, argv, "-j");
    logVerbose("blame for: " + filePath + " @ " + headSha);

    auto blame = ghost::git::Blame::getLineAuthorMap(filePath);
    if (blame.empty()) {
        std::cout << ghost::output::Style::warning("No blame data for " + filePath) << "\n";
        return GHOST_EXIT_OK;
    }

    std::map<std::string, ghost::note::NoteReader::Result> ghostNotes;
    std::string rawNote = ghost::git::Notes::show("refs/notes/ghost", headSha);
    if (!rawNote.empty()) {
        ghostNotes[headSha] = ghost::note::NoteReader::parse(rawNote);
    }

    auto attribution = ghost::audit::BlameOverlay::overlay(filePath, blame, ghostNotes);

    if (jsonOutput) {
        std::cout << "{\n";
        std::cout << "  \"file\": \"" << filePath << "\",\n";
        std::cout << "  \"total_lines\": " << attribution.total_lines << ",\n";
        std::cout << "  \"ai_lines\": " << attribution.ai_lines << ",\n";
        std::cout << "  \"lines\": [\n";
        for (size_t i = 0; i < attribution.lines.size(); ++i) {
            const auto& l = attribution.lines[i];
            std::cout << "    {\"line\": " << l.line_number
                      << ", \"commit\": \"" << l.commit_sha
                      << "\", \"is_ai\": " << (l.is_ai ? "true" : "false");
            if (l.is_ai) {
                std::cout << ", \"agent\": \"" << l.agent
                          << "\", \"model\": \"" << l.model << "\"";
            }
            std::cout << "}";
            if (i + 1 < attribution.lines.size()) std::cout << ",";
            std::cout << "\n";
        }
        std::cout << "  ]\n";
        std::cout << "}\n";
    } else {
        bool hasTerm = std::getenv("TERM") != nullptr && std::getenv("NO_COLOR") == nullptr;
        auto v = [&](const std::string& s) { return hasTerm ? "\033[38;5;141m" + s + "\033[0m" : s; };
        auto b = [&](const std::string& s) { return hasTerm ? "\033[38;5;75m" + s + "\033[0m" : s; };
        auto w = [&](const std::string& s) { return hasTerm ? "\033[38;5;231m" + s + "\033[0m" : s; };
        auto d = [&](const std::string& s) { return hasTerm ? "\033[2m\033[38;5;248m" + s + "\033[0m" : s; };
        for (const auto& l : attribution.lines) {
            std::string tag = l.is_ai ? v("AI  ") : d("human");
            std::cout << d(std::to_string(l.line_number)) << " "
                      << b(l.commit_sha.substr(0, 8)) << " "
                      << tag;
            if (l.is_ai) {
                std::cout << " " << d("|") << " " << w(l.agent) << " " << d("/") << " " << w(l.model);
            }
            std::cout << "\n";
        }
        int pct = attribution.total_lines > 0
            ? (attribution.ai_lines * 100) / attribution.total_lines : 0;
        std::cout << "\n" << d(std::to_string(attribution.ai_lines) + "/" + std::to_string(attribution.total_lines))
                  << " AI lines (" << v(std::to_string(pct) + "%") << ")\n";
    }
    return GHOST_EXIT_OK;
}

static int handleStats(int argc, char* argv[]) {
    std::string repoRoot = ghost::git::Repo::getRoot();
    if (repoRoot.empty()) {
        std::cerr << ghost::output::Style::error("Not in a git repository") << "\n";
        return GHOST_EXIT_NOT_IN_REPO;
    }
    std::string range = "HEAD~1..HEAD";
    if (argc > 2 && std::string(argv[2])[0] != '-') {
        range = argv[2];
    }
    bool jsonOutput = hasFlag(argc, argv, "--json") || hasFlag(argc, argv, "-j");
    logVerbose("stats range: " + range);

    auto report = ghost::audit::Auditor::run(repoRoot, range, -1, false);
    if (jsonOutput) {
        std::cout << "{\n";
        std::cout << "  \"total_commits\": " << report.summary.commits.size() << ",\n";
        std::cout << "  \"total_lines\": " << report.summary.total_lines << ",\n";
        std::cout << "  \"ai_lines\": " << report.summary.ai_lines << ",\n";
        std::cout << "  \"ai_percent\": " << (report.summary.total_lines > 0
            ? (report.summary.ai_lines * 100.0) / report.summary.total_lines : 0.0) << ",\n";
        std::cout << "  \"commits\": [\n";
        for (size_t i = 0; i < report.summary.commits.size(); ++i) {
            const auto& c = report.summary.commits[i];
            double cpct = c.total_lines > 0 ? (c.ai_lines * 100.0) / c.total_lines : 0.0;
            std::cout << "    {\"commit\": \"" << c.commit_sha
                      << "\", \"ai_lines\": " << c.ai_lines
                      << ", \"total_lines\": " << c.total_lines
                      << ", \"ai_percent\": " << cpct << "}";
            if (i + 1 < report.summary.commits.size()) std::cout << ",";
            std::cout << "\n";
        }
        std::cout << "  ]\n";
        std::cout << "}\n";
    } else {
        bool hasTerm = std::getenv("TERM") != nullptr && std::getenv("NO_COLOR") == nullptr;
        auto v = [&](const std::string& s) { return hasTerm ? "\033[38;5;141m" + s + "\033[0m" : s; };
        auto b = [&](const std::string& s) { return hasTerm ? "\033[38;5;75m" + s + "\033[0m" : s; };
        auto d = [&](const std::string& s) { return hasTerm ? "\033[2m\033[38;5;248m" + s + "\033[0m" : s; };
        for (const auto& c : report.summary.commits) {
            int cpct = c.total_lines > 0 ? (c.ai_lines * 100) / c.total_lines : 0;
            std::cout << "  " << b(c.commit_sha.substr(0, 8)) << "  "
                      << v(std::to_string(cpct) + "%") << " "
                      << d("(" + std::to_string(c.ai_lines) + "/" + std::to_string(c.total_lines) + " lines)") << "\n";
        }
        if (report.summary.commits.size() > 1) {
            int apct = report.summary.total_lines > 0
                ? (report.summary.ai_lines * 100) / report.summary.total_lines : 0;
            std::cout << "\n  " << d("total") << "  " << v(std::to_string(apct) + "%") << " "
                      << d("(" + std::to_string(report.summary.ai_lines) + "/" + std::to_string(report.summary.total_lines) + " lines)") << "\n";
        }
    }
    return GHOST_EXIT_OK;
}

static int handleConfig(int argc, char* argv[]) {
    std::string repoRoot = ghost::git::Repo::getRoot();
    if (repoRoot.empty()) {
        std::cerr << ghost::output::Style::error("Not in a git repository") << "\n";
        return GHOST_EXIT_NOT_IN_REPO;
    }
    if (argc > 2 && std::string(argv[2]) == "set" && argc >= 5) {
        logVerbose("config set: " + std::string(argv[3]) + " = " + argv[4]);
        if (ghost::config::GhostConfigReader::save(repoRoot, argv[3], argv[4])) {
            std::cout << ghost::output::Style::success("Set " + std::string(argv[3]) + " = " + argv[4]) << "\n";
        } else {
            std::cerr << ghost::output::Style::error("Failed to write ghost.yml") << "\n";
            return GHOST_EXIT_ERROR;
        }
    } else {
        auto cfg = ghost::config::GhostConfigReader::load(repoRoot);
        bool hasTerm = std::getenv("TERM") != nullptr && std::getenv("NO_COLOR") == nullptr;
        auto v = [&](const std::string& s) { return hasTerm ? "\033[38;5;141m" + s + "\033[0m" : s; };
        auto b = [&](const std::string& s) { return hasTerm ? "\033[38;5;75m" + s + "\033[0m" : s; };
        auto w = [&](const std::string& s) { return hasTerm ? "\033[38;5;231m" + s + "\033[0m" : s; };
        auto d = [&](const std::string& s) { return hasTerm ? "\033[2m\033[38;5;248m" + s + "\033[0m" : s; };
        auto g = [&](const std::string& s) { return hasTerm ? "\033[32m" + s + "\033[0m" : s; };
        std::cout << b("version") << "    " << w(std::to_string(cfg.version)) << "\n";
        std::cout << b("required") << "   " << (cfg.required ? g("true") : d("false")) << "\n";
        std::cout << b("threshold") << "  " << w(std::to_string(cfg.threshold)) << "\n";
        std::cout << b("on_exceed") << "  " << w(cfg.on_exceed) << "\n";
        std::cout << b("pr_comment") << " " << (cfg.pr_comment ? g("true") : d("false")) << "\n";
        std::cout << b("untagged") << "   " << w(cfg.untagged_policy) << "\n";
        std::cout << b("unverified") << " " << w(cfg.unverified_policy) << "\n";
        std::cout << b("gitai_fb") << "   " << (cfg.gitai_fallback ? g("true") : d("false")) << "\n";
        if (!cfg.ignore.empty()) {
            std::cout << b("ignore") << "     " << w(cfg.ignore[0]);
            for (size_t i = 1; i < cfg.ignore.size(); ++i) {
                std::cout << d(", ") << w(cfg.ignore[i]);
            }
            std::cout << "\n";
        }
    }
    return GHOST_EXIT_OK;
}

static int handleInstallHooks(int argc, char* argv[]) {
    std::string repoRoot = ghost::git::Repo::getRoot();
    if (repoRoot.empty()) {
        std::cerr << ghost::output::Style::error("Not in a git repository") << "\n";
        return GHOST_EXIT_NOT_IN_REPO;
    }
    bool global = !hasFlag(argc, argv, "--repo");
    std::string specificAgent;
    for (int i = 2; i < argc - 1; ++i) {
        if (std::string(argv[i]) == "--agent" && i + 1 < argc) {
            specificAgent = argv[i + 1];
            break;
        }
    }

    logVerbose("install hooks, global=" + std::to_string(global) + ", agent=" + specificAgent);
    if (!specificAgent.empty()) {
        if (!ghost::hooks::AgentHooks::installForAgent(repoRoot, specificAgent, global)) return GHOST_EXIT_ERROR;
    } else {
        if (!ghost::hooks::AgentHooks::installAll(repoRoot, global)) return GHOST_EXIT_ERROR;
    }
    return GHOST_EXIT_OK;
}

static int handleUninstallHooks(int argc, char* argv[]) {
    std::string repoRoot = ghost::git::Repo::getRoot();
    if (repoRoot.empty()) {
        std::cerr << ghost::output::Style::error("Not in a git repository") << "\n";
        return GHOST_EXIT_NOT_IN_REPO;
    }
    bool global = !hasFlag(argc, argv, "--repo");
    std::string specificAgent;
    for (int i = 2; i < argc - 1; ++i) {
        if (std::string(argv[i]) == "--agent" && i + 1 < argc) {
            specificAgent = argv[i + 1];
            break;
        }
    }

    logVerbose("uninstall hooks, global=" + std::to_string(global) + ", agent=" + specificAgent);
    if (!specificAgent.empty()) {
        ghost::hooks::AgentHooks::uninstallForAgent(repoRoot, specificAgent, global);
    } else {
        ghost::hooks::AgentHooks::uninstallAll(repoRoot, global);
    }
    return GHOST_EXIT_OK;
}

static int handleInit(int argc, char* argv[]) {
    std::string repoRoot = ghost::git::Repo::getRoot();
    if (repoRoot.empty()) {
        std::cerr << ghost::output::Style::error("Not in a git repository") << "\n";
        return GHOST_EXIT_NOT_IN_REPO;
    }

    bool yesMode = hasFlag(argc, argv, "--yes") || hasFlag(argc, argv, "-y");
    bool interactive = hasFlag(argc, argv, "--interactive") || hasFlag(argc, argv, "-i");
    bool dryRun = hasFlag(argc, argv, "--dry-run") || hasFlag(argc, argv, "-n");

    logVerbose("init repo=" + repoRoot + " yes=" + std::to_string(yesMode) +
               " interactive=" + std::to_string(interactive) + " dryRun=" + std::to_string(dryRun));

    using namespace ghost::output;
    using namespace ghost::output::interactive;

    // Default config values
    int threshold = 80;
    bool required = false;
    std::string onExceed = "block";
    bool prComment = true;
    std::string untaggedPolicy = "human";
    std::string unverifiedPolicy = "warn";
    bool gitaiFallback = true;
    std::vector<std::string> ignorePatterns;
    std::vector<std::string> selectedAgents;

    // Smart defaults: detect common build dirs
    if (fileExists(repoRoot + "/node_modules")) {
        ignorePatterns.push_back("node_modules/");
    }
    if (fileExists(repoRoot + "/build")) {
        ignorePatterns.push_back("build/");
    }
    if (fileExists(repoRoot + "/dist")) {
        ignorePatterns.push_back("dist/");
    }
    if (fileExists(repoRoot + "/target")) {
        ignorePatterns.push_back("target/");  // Rust
    }
    if (fileExists(repoRoot + "/.next")) {
        ignorePatterns.push_back(".next/");
    }
    ignorePatterns.push_back(".git/");

    // Interactive wizard
    if (interactive && isInteractive()) {
        std::cout << Style::header("Ghost Setup Wizard") << "\n";
        std::cout << Style::dim("  Configure ghost for this repository.\n\n");

        // Step 1: Threshold
        std::vector<std::string> thresholdOpts = {"80% (default)", "50% (strict)", "30% (very strict)", "Custom"};
        int threshChoice = selectMenu("AI attribution threshold", thresholdOpts, 0);
        if (threshChoice < 0) return GHOST_EXIT_ERROR;
        if (threshChoice == 3) {
            std::string custom = inputPrompt("Enter threshold percentage (0-100)", "80");
            try { threshold = std::stoi(custom); } catch (...) { threshold = 80; }
        } else if (threshChoice == 1) {
            threshold = 50;
        } else if (threshChoice == 2) {
            threshold = 30;
        }

        // Step 2: Required
        std::vector<std::string> reqOpts = {"No (permissive)", "Yes (enforce on push)"};
        int reqChoice = selectMenu("Require ghost attribution?", reqOpts, 0);
        if (reqChoice < 0) return GHOST_EXIT_ERROR;
        required = (reqChoice == 1);

        // Step 3: On exceed
        std::vector<std::string> exceedOpts = {"block", "warn", "allow"};
        int exceedChoice = selectMenu("Policy when AI% exceeds threshold", exceedOpts, 0);
        if (exceedChoice < 0) return GHOST_EXIT_ERROR;
        onExceed = exceedOpts[exceedChoice];

        // Step 4: Agents
        auto detected = ghost::hooks::AgentDetector::detectInstalled();
        if (!detected.empty()) {
            std::cout << "\n" << Style::bold(Style::blue("Detected Agents")) << "\n";
            for (const auto& a : detected) {
                std::cout << "  " << Style::violet(a) << "\n";
            }
            std::cout << "\n";

            bool installAll = confirmPrompt("Install hooks for all detected agents?", true);
            if (!installAll) {
                for (const auto& a : detected) {
                    if (confirmPrompt("Install hook for " + a + "?", true)) {
                        selectedAgents.push_back(a);
                    }
                }
            } else {
                for (const auto& a : detected) {
                    selectedAgents.push_back(a);
                }
            }
        }

        // Step 5: Ignore patterns
        std::cout << "\n" << Style::bold(Style::blue("Ignore Patterns")) << "\n";
        std::cout << "  Pre-filled based on repo contents:\n";
        for (const auto& p : ignorePatterns) {
            std::cout << "    " << Style::dim("- " + p) << "\n";
        }
        if (confirmPrompt("Add custom ignore patterns?", false)) {
            std::string custom = inputPrompt("Enter pattern (e.g., '*.min.js', leave empty to finish)");
            while (!custom.empty()) {
                ignorePatterns.push_back(custom);
                custom = inputPrompt("Another pattern (leave empty to finish)");
            }
        }
    }

    // Dry run preview
    if (dryRun) {
        std::cout << Style::header("Dry Run — ghost init");
        std::cout << "Would configure:\n";
        std::cout << "  - ghost.yml (threshold=" << threshold << ", required=" << (required ? "true" : "false") << ")\n";
        std::cout << "  - post-commit hook\n";
        std::cout << "  - pre-push hook\n";
        std::cout << "  - git notes push refs\n";
        if (!selectedAgents.empty()) {
            std::cout << "  - agent hooks for: ";
            for (size_t i = 0; i < selectedAgents.size(); ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << selectedAgents[i];
            }
            std::cout << "\n";
        }
        if (yesMode) {
            std::cout << "  - binaries to ~/.ghost/bin (if not in PATH)\n";
        }
        std::cout << "\n";
        return GHOST_EXIT_OK;
    }

    // Write ghost.yml
    std::string ymlPath = repoRoot + "/ghost.yml";
    {
        std::ofstream yml(ymlPath);
        if (!yml) {
            std::cerr << Style::error("Failed to write ghost.yml") << "\n";
            return GHOST_EXIT_ERROR;
        }
        yml << "# Ghost configuration\n";
        yml << "# See: https://github.com/farhankhan197/ghost#configuration\n";
        yml << "\n";
        yml << "version: 1\n";
        yml << "threshold: " << threshold << "\n";
        yml << "required: " << (required ? "true" : "false") << "\n";
        yml << "on_exceed: " << onExceed << "\n";
        yml << "pr_comment: " << (prComment ? "true" : "false") << "\n";
        yml << "untagged: " << untaggedPolicy << "\n";
        yml << "unverified: " << unverifiedPolicy << "\n";
        yml << "gitai_fb: " << (gitaiFallback ? "true" : "false") << "\n";
        if (!ignorePatterns.empty()) {
            yml << "ignore:\n";
            for (const auto& p : ignorePatterns) {
                yml << "  - " << p << "\n";
            }
        }
    }
    std::cout << "  " << Style::success("Created ghost.yml") << "\n";
    logVerbose("wrote ghost.yml to " + ymlPath);

    // Install hooks (but not binaries - init is hooks-only)
    int hooksResult = ghost::hooks::Installer::installRepo(repoRoot);
    if (hooksResult != GHOST_EXIT_OK) {
        std::cerr << Style::warning("Warning: some hooks may not have installed correctly") << "\n";
    }

    // Install agent hooks
    if (!selectedAgents.empty()) {
        for (const auto& agent : selectedAgents) {
            if (ghost::hooks::AgentHooks::installForAgent(repoRoot, agent, false)) {
                std::cout << "  " << Style::success("Installed hook for " + agent) << "\n";
            } else {
                std::cerr << Style::warning("  Could not install hook for " + agent) << "\n";
            }
        }
    } else if (interactive) {
        // In interactive mode without agents selected, still install the opencode plugin
        // since it's the default and works for all repos
        if (ghost::hooks::AgentHooks::installAll(repoRoot, false)) {
            std::cout << "  " << Style::success("Installed default agent hooks") << "\n";
        }
    }

    // Optionally install binaries
    if (yesMode) {
        std::string ghostPath = execCommand("which ghost 2>/dev/null || where ghost 2>nul");
        if (ghostPath.empty() || ghostPath.find("not found") != std::string::npos) {
            std::cout << "  " << Style::dim("ghost not found in PATH, installing binaries...") << "\n";
            int binResult = ghost::hooks::Installer::installBin();
            if (binResult == GHOST_EXIT_OK) {
                std::cout << "  " << Style::success("Installed binaries to ~/.ghost/bin") << "\n";
                std::cout << "  " << Style::warning("Add ~/.ghost/bin to your PATH to use ghost from anywhere") << "\n";
            } else {
                std::cerr << Style::warning("  Failed to install binaries. Run 'ghost install' later.") << "\n";
            }
        }
    }

    std::cout << "\n" << Style::success("Done. Ghost is initialized in this repo.") << "\n";
    if (!yesMode) {
        std::cout << Style::dim("  Run 'ghost install' to install binaries if needed.\n");
    }
    std::cout << "\n";
    return GHOST_EXIT_OK;
}

static int handleDoctor(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    using namespace ghost::output;

    std::cout << Style::header("Ghost Doctor");

    bool allOk = true;
    bool autoFix = hasFlag(argc, argv, "--fix") || hasFlag(argc, argv, "-f");

    // Check 1: Git repository
    std::string repoRoot = ghost::git::Repo::getRoot();
    if (repoRoot.empty()) {
        std::cout << "  " << Style::error("✗ Not in a git repository") << "\n";
        return GHOST_EXIT_ERROR;
    }
    std::cout << "  " << Style::success("✓ Git repository") << " " << Style::dim(repoRoot) << "\n";

    // Check 2: ghost binary in PATH
    {
        std::string ghostPath = execCommand("which ghost 2>/dev/null || where ghost 2>nul");
        if (ghostPath.empty() || ghostPath.find("not found") != std::string::npos) {
            std::cout << "  " << Style::warning("⚠ ghost not in PATH") << "\n";
            std::cout << "    " << Style::dim("Run 'ghost install' or add ~/.ghost/bin to PATH") << "\n";
            allOk = false;
        } else {
            std::cout << "  " << Style::success("✓ ghost in PATH") << " " << Style::dim(ghostPath) << "\n";
        }
    }

    // Check 3: ghost.yml exists
    std::string ymlPath = repoRoot + "/ghost.yml";
    bool ymlExists = fileExists(ymlPath);
    if (!ymlExists) {
        std::cout << "  " << Style::warning("⚠ ghost.yml not found") << "\n";
        if (autoFix) {
            ghost::config::GhostConfigReader::save(repoRoot, "threshold", "80");
            std::cout << "    " << Style::success("Fixed: created ghost.yml with defaults") << "\n";
            ymlExists = true;
        } else {
            std::cout << "    " << Style::dim("Run 'ghost init' to create one") << "\n";
            allOk = false;
        }
    } else {
        auto cfg = ghost::config::GhostConfigReader::load(repoRoot);
        std::cout << "  " << Style::success("✓ ghost.yml") << " "
                  << Style::dim("threshold=" + std::to_string(cfg.threshold) +
                                  ", required=" + (cfg.required ? "true" : "false")) << "\n";
    }

    // Check 4: Hooks exist
    std::string postCommitHook = repoRoot + "/.git/hooks/post-commit";
    std::string prePushHook = repoRoot + "/.git/hooks/pre-push";
    bool postCommitExists = fileExists(postCommitHook);
    bool prePushExists = fileExists(prePushHook);

    if (!postCommitExists) {
        std::cout << "  " << Style::warning("⚠ post-commit hook missing") << "\n";
        if (autoFix) {
            ghost::hooks::Installer::installRepo(repoRoot);
            std::cout << "    " << Style::success("Fixed: installed hooks") << "\n";
        } else {
            allOk = false;
        }
    } else {
        std::cout << "  " << Style::success("✓ post-commit hook") << "\n";
    }

    if (!prePushExists) {
        std::cout << "  " << Style::warning("⚠ pre-push hook missing") << "\n";
        if (autoFix) {
            // Installer::installRepo installs both
            if (!postCommitExists) ghost::hooks::Installer::installRepo(repoRoot);
        } else {
            allOk = false;
        }
    } else {
        std::cout << "  " << Style::success("✓ pre-push hook") << "\n";
    }

    // Check 5: Git notes refs configured
    {
        std::string remotePush = execCommand("git config --get remote.origin.push 2>/dev/null || echo ''");
        bool hasNotesRef = (remotePush.find("refs/notes/ghost") != std::string::npos);
        if (!hasNotesRef) {
            std::cout << "  " << Style::warning("⚠ git notes push not configured") << "\n";
            if (autoFix) {
                execCommand("git config --add remote.origin.push \"+refs/notes/ghost:refs/notes/ghost\"");
                execCommand("git config --add remote.origin.push \"+refs/notes/ghost-verified:refs/notes/ghost-verified\"");
                std::cout << "    " << Style::success("Fixed: configured notes push") << "\n";
            } else {
                std::cout << "    " << Style::dim("Run 'git config --add remote.origin.push +refs/notes/ghost:refs/notes/ghost'") << "\n";
                allOk = false;
            }
        } else {
            std::cout << "  " << Style::success("✓ git notes push configured") << "\n";
        }
    }

    // Check 6: Agent plugins
    auto detected = ghost::hooks::AgentDetector::detectInstalled();
    if (detected.empty()) {
        std::cout << "  " << Style::dim("  No AI agents detected") << "\n";
    } else {
        for (const auto& a : detected) {
            std::string agentDir = ghost::hooks::AgentDetector::getRepoConfigDir(a, repoRoot);
            bool hasHook = !agentDir.empty() && fileExists(agentDir);
            if (hasHook) {
                std::cout << "  " << Style::success("✓ " + a + " hook") << "\n";
            } else {
                std::cout << "  " << Style::warning("⚠ " + a + " detected but hook not installed") << "\n";
                if (autoFix) {
                    if (ghost::hooks::AgentHooks::installForAgent(repoRoot, a, false)) {
                        std::cout << "    " << Style::success("Fixed: installed " + a + " hook") << "\n";
                    }
                } else {
                    allOk = false;
                }
            }
        }
    }

    std::cout << "\n";
    if (allOk) {
        std::cout << Style::success("  All checks passed!") << "\n\n";
    } else {
        std::cout << Style::warning("  Some issues found. Run 'ghost doctor --fix' to auto-fix.") << "\n\n";
    }
    return allOk ? GHOST_EXIT_OK : GHOST_EXIT_ERROR;
}

static int handleStatus(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    using namespace ghost::output;

    std::string repoRoot = ghost::git::Repo::getRoot();
    if (repoRoot.empty()) {
        std::cerr << Style::error("Not in a git repository") << "\n";
        return GHOST_EXIT_NOT_IN_REPO;
    }

    std::cout << Style::header("Ghost Status");

    // Config
    auto cfg = ghost::config::GhostConfigReader::load(repoRoot);
    std::cout << Style::bold(Style::blue("  Configuration")) << "\n";
    std::cout << "    threshold:   " << Style::glow(std::to_string(cfg.threshold) + "%") << "\n";
    std::cout << "    required:    " << (cfg.required ? Style::success("true") : Style::dim("false")) << "\n";
    std::cout << "    on_exceed:   " << Style::glow(cfg.on_exceed) << "\n";
    if (!cfg.ignore.empty()) {
        std::cout << "    ignore:      " << Style::dim(cfg.ignore[0]);
        for (size_t i = 1; i < cfg.ignore.size(); ++i) {
            std::cout << Style::dim(", " + cfg.ignore[i]);
        }
        std::cout << "\n";
    }
    std::cout << "\n";

    // Hooks
    std::cout << Style::bold(Style::blue("  Hooks")) << "\n";
    bool postCommit = fileExists(repoRoot + "/.git/hooks/post-commit");
    bool prePush = fileExists(repoRoot + "/.git/hooks/pre-push");
    std::cout << "    post-commit: " << (postCommit ? Style::success("installed") : Style::warning("missing")) << "\n";
    std::cout << "    pre-push:    " << (prePush ? Style::success("installed") : Style::warning("missing")) << "\n";

    // Active sessions
    std::string ghostDir = repoRoot + "/.git/ghost";
    bool hasPreState = fileExists(ghostDir + "/working.log");
    std::cout << "\n" << Style::bold(Style::blue("  Sessions")) << "\n";
    if (hasPreState) {
        std::cout << "    " << Style::warning("Active checkpoint session detected") << "\n";
        std::cout << "    " << Style::dim("Run 'ghost-checkpoint show' for details") << "\n";
    } else {
        std::cout << "    " << Style::dim("No active sessions") << "\n";
    }

    // Notes summary
    std::string headSha = ghost::git::Repo::getHead();
    std::string note = ghost::git::Notes::show("refs/notes/ghost", headSha);
    std::cout << "\n" << Style::bold(Style::blue("  Latest Commit")) << "\n";
    std::cout << "    " << Style::violet(headSha.substr(0, 8)) << " ";
    if (!note.empty()) {
        auto parsed = ghost::note::NoteReader::parse(note);
        if (parsed.success) {
            int totalEntries = 0;
            for (const auto& e : parsed.entries) totalEntries++;
            std::cout << Style::success("ghost note present") << " " << Style::dim("(" + std::to_string(totalEntries) + " files)") << "\n";
        } else {
            std::cout << Style::warning("ghost note present but unparsable") << "\n";
        }
    } else {
        std::cout << Style::dim("no ghost note") << "\n";
    }

    std::cout << "\n";
    return GHOST_EXIT_OK;
}

static int handleCheck(int argc, char* argv[]) {
    std::string repoRoot = ghost::git::Repo::getRoot();
    if (repoRoot.empty()) {
        std::cerr << ghost::output::Style::error("Not in a git repository") << "\n";
        return GHOST_EXIT_NOT_IN_REPO;
    }

    bool jsonOutput = hasFlag(argc, argv, "--json") || hasFlag(argc, argv, "-j");
    logVerbose("checking staged changes");

    using namespace ghost::output;

    // Get staged diff stats
    auto stagedFiles = ghost::git::Diff::getChangedFiles("--cached");
    if (stagedFiles.empty()) {
        std::cout << Style::warning("No staged changes to check") << "\n";
        std::cout << Style::dim("Run 'git add <files>' first, or use 'ghost audit' for committed changes.") << "\n\n";
        return GHOST_EXIT_OK;
    }

    // Check for active checkpoint session
    bool hasActiveSession = fileExists(repoRoot + "/.git/ghost/working.log");
    std::string sessionAgent = "unknown";
    std::string sessionModel = "unknown";
    if (hasActiveSession) {
        auto preState = ghost::checkpoint::WorkingLog::loadPreState(repoRoot);
        if (preState.valid) {
            sessionAgent = preState.agent;
        }
        // Try to read current model
        std::string modelFile = repoRoot + "/.git/ghost/.current_model";
        if (fileExists(modelFile)) {
            std::ifstream mf(modelFile);
            std::getline(mf, sessionModel);
        }
    }

    // Load ghost notes for HEAD (for predicting modifications to existing AI lines)
    std::string headSha = ghost::git::Repo::getHead();
    std::map<std::string, ghost::note::NoteReader::Result> ghostNotes;
    std::string rawNote = ghost::git::Notes::show("refs/notes/ghost", headSha);
    if (!rawNote.empty()) {
        ghostNotes[headSha] = ghost::note::NoteReader::parse(rawNote);
    }

    // Compute predictions per file
    int totalAdditions = 0;
    int predictedAiAdditions = 0;

    struct FilePrediction {
        std::string path;
        int additions;
        int deletions;
        int predictedAiAdditions;
        std::string reason;
    };
    std::vector<FilePrediction> predictions;

    for (const auto& df : stagedFiles) {
        FilePrediction pred;
        pred.path = df.path;
        pred.additions = df.additions;
        pred.deletions = df.deletions;
        totalAdditions += df.additions;

        // Determine if this file is likely AI-authored based on active session
        if (hasActiveSession) {
            pred.predictedAiAdditions = df.additions;
            pred.reason = "active session: " + sessionAgent + "/" + sessionModel;
        } else {
            // No active session: check if file has existing AI attribution in HEAD
            if (ghostNotes.count(headSha)) {
                const auto& note = ghostNotes[headSha];
                bool hasAiHistory = false;
                for (const auto& entry : note.entries) {
                    if (entry.file_path == df.path) {
                        hasAiHistory = true;
                        break;
                    }
                }
                if (hasAiHistory) {
                    // File previously had AI lines; modifications likely still AI
                    pred.predictedAiAdditions = df.additions;
                    pred.reason = "file has prior AI attribution";
                } else {
                    pred.predictedAiAdditions = 0;
                    pred.reason = "no active session, no prior AI";
                }
            } else {
                pred.predictedAiAdditions = 0;
                pred.reason = "no active session, no prior AI";
            }
        }
        predictedAiAdditions += pred.predictedAiAdditions;
        predictions.push_back(pred);
    }

    double aiPercent = totalAdditions > 0 ? (predictedAiAdditions * 100.0) / totalAdditions : 0.0;

    // Load config for threshold check
    auto cfg = ghost::config::GhostConfigReader::load(repoRoot);
    bool wouldPass = true;
    std::string statusMsg = "WOULD PASS";
    if (cfg.threshold > 0 && aiPercent > cfg.threshold) {
        if (cfg.on_exceed == "block") {
            wouldPass = false;
            statusMsg = "WOULD FAIL (exceeds threshold)";
        } else if (cfg.on_exceed == "warn") {
            statusMsg = "WOULD WARN (exceeds threshold)";
        }
    }

    if (jsonOutput) {
        std::cout << "{\n";
        std::cout << "  \"staged_files\": " << stagedFiles.size() << ",\n";
        std::cout << "  \"total_additions\": " << totalAdditions << ",\n";
        std::cout << "  \"predicted_ai_additions\": " << predictedAiAdditions << ",\n";
        std::cout << "  \"predicted_ai_percent\": " << aiPercent << ",\n";
        std::cout << "  \"threshold\": " << cfg.threshold << ",\n";
        std::cout << "  \"would_pass\": " << (wouldPass ? "true" : "false") << ",\n";
        std::cout << "  \"status\": \"" << statusMsg << "\",\n";
        std::cout << "  \"files\": [\n";
        for (size_t i = 0; i < predictions.size(); ++i) {
            const auto& p = predictions[i];
            std::cout << "    {\"path\": \"" << p.path << "\", ";
            std::cout << "\"additions\": " << p.additions << ", ";
            std::cout << "\"deletions\": " << p.deletions << ", ";
            std::cout << "\"predicted_ai_additions\": " << p.predictedAiAdditions << ", ";
            std::cout << "\"reason\": \"" << p.reason << "\"}";
            if (i + 1 < predictions.size()) std::cout << ",";
            std::cout << "\n";
        }
        std::cout << "  ]\n";
        std::cout << "}\n";
    } else {
        std::cout << Style::header("Predicted Commit Attribution");
        std::cout << "  " << Style::dim(std::to_string(stagedFiles.size()) + " file" + (stagedFiles.size() == 1 ? "" : "s") + " staged") << "\n\n";

        auto v = Style::violet;
        auto d = Style::dim;
        auto g = Style::glow;

        for (const auto& p : predictions) {
            int pct = p.additions > 0 ? (p.predictedAiAdditions * 100) / p.additions : 0;
            std::cout << "  " << Style::padRight(Style::blue(p.path), 30);
            std::cout << Style::padRight(std::to_string(p.additions) + "+ " + std::to_string(p.deletions) + "-", 12);
            std::cout << Style::progressBar(pct, 100, 10) << " ";
            if (pct > 0) {
                std::cout << v(std::to_string(pct) + "%") << " " << d(p.reason);
            } else {
                std::cout << d("0%") << " " << d(p.reason);
            }
            std::cout << "\n";
        }

        std::cout << "\n  " << d("Policy: threshold " + std::to_string(cfg.threshold) + "%")
                  << "  |  ";
        if (wouldPass) {
            std::cout << Style::success(statusMsg) << " " << Style::success("✓");
        } else {
            std::cout << Style::error(statusMsg) << " " << Style::error("✗");
        }
        std::cout << "\n";
        std::cout << "  " << d("Run 'ghost audit' after committing to verify.") << "\n\n";
    }

    return wouldPass ? GHOST_EXIT_OK : GHOST_EXIT_BLOCKED;
}

static int handlePostCommit(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    std::string repoRoot = ghost::git::Repo::getRoot();
    std::string commitSha = ghost::git::Repo::getHead();
    if (repoRoot.empty() || commitSha.empty()) {
        std::cerr << ghost::output::Style::error("Not in a git repository") << "\n";
        return GHOST_EXIT_NOT_IN_REPO;
    }
    logVerbose("post-commit for: " + commitSha);
    return ghost::commit::PostCommit::run(repoRoot, commitSha);
}

static int handleCompletion(int argc, char* argv[]) {
    if (argc < 3) {
        ghost::cli::CommandRegistry::printHelp("completion");
        return GHOST_EXIT_ERROR;
    }
    std::string shell = argv[2];
    logVerbose("generating completions for: " + shell);
    
    if (shell == "bash") {
        std::cout << R"(
_ghost_completions() {
    local cur prev opts
    COMPREPLY=()
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD-1]}"
    
    opts="init install uninstall audit check blame show stats config doctor status install-hooks uninstall-hooks completion version help --global --dry-run --json --all --range --threshold --verbose"
    
    case "${prev}" in
        ghost)
            COMPREPLY=( $(compgen -W "${opts}" -- ${cur}) )
            return 0
            ;;
        install|uninstall)
            COMPREPLY=( $(compgen -W "--global --dry-run" -- ${cur}) )
            return 0
            ;;
        audit)
            COMPREPLY=( $(compgen -W "--all --range --threshold --json" -- ${cur}) )
            return 0
            ;;
        *)
            ;;
    esac
    
    COMPREPLY=( $(compgen -W "${opts}" -- ${cur}) )
    return 0
}

complete -F _ghost_completions ghost
)";
    } else if (shell == "zsh") {
        std::cout << R"(
#compdef ghost

_ghost() {
    local -a commands
    commands=(
        'init:Initialize ghost in the current repository'
        'install:Install ghost in the current repository'
        'uninstall:Remove ghost from the current repository'
        'install-hooks:Auto-configure AI agent hooks'
        'uninstall-hooks:Remove AI agent hooks'
        'audit:Run AI attribution audit'
        'check:Predictive pre-commit audit'
        'blame:Line-by-line attribution'
        'show:Show raw ghost note'
        'stats:AI% statistics'
        'config:Show/set ghost.yml configuration'
        'doctor:Diagnose ghost setup and suggest fixes'
        'status:Show ghost status overview'
        'completion:Generate shell completion script'
        'version:Print version information'
        'help:Show help'
    )
    
    _arguments -C \
        '1: :->command' \
        '*: :->args' && ret=0
    
    case "$state" in
        command)
            _describe -t commands 'ghost commands' commands && ret=0
            ;;
        args)
            case "$line[1]" in
                install|uninstall)
                    _arguments '--global[Global installation]'
                    ;;
                audit)
                    _arguments '--all[Audit all commits]' '--range[Commit range]:range:' '--threshold[Threshold]:threshold:' '--json[JSON output]'
                    ;;
            esac
            ;;
    esac
}

compdef _ghost ghost
)";
    } else if (shell == "fish") {
        std::cout << R"(
# Ghost completion for fish shell

complete -c ghost -f

# Commands
complete -c ghost -n '__fish_use_subcommand' -a init -d 'Initialize ghost in the current repository'
complete -c ghost -n '__fish_use_subcommand' -a install -d 'Install ghost in the current repository'
complete -c ghost -n '__fish_use_subcommand' -a uninstall -d 'Remove ghost from the current repository'
complete -c ghost -n '__fish_use_subcommand' -a install-hooks -d 'Auto-configure AI agent hooks'
complete -c ghost -n '__fish_use_subcommand' -a uninstall-hooks -d 'Remove AI agent hooks'
complete -c ghost -n '__fish_use_subcommand' -a audit -d 'Run AI attribution audit'
complete -c ghost -n '__fish_use_subcommand' -a check -d 'Predictive pre-commit audit'
complete -c ghost -n '__fish_use_subcommand' -a blame -d 'Line-by-line attribution'
complete -c ghost -n '__fish_use_subcommand' -a show -d 'Show raw ghost note'
complete -c ghost -n '__fish_use_subcommand' -a stats -d 'AI% statistics'
complete -c ghost -n '__fish_use_subcommand' -a config -d 'Show/set ghost.yml configuration'
complete -c ghost -n '__fish_use_subcommand' -a doctor -d 'Diagnose ghost setup and suggest fixes'
complete -c ghost -n '__fish_use_subcommand' -a status -d 'Show ghost status overview'
complete -c ghost -n '__fish_use_subcommand' -a completion -d 'Generate shell completion script'
complete -c ghost -n '__fish_use_subcommand' -a version -d 'Print version information'
complete -c ghost -n '__fish_use_subcommand' -a help -d 'Show help'

# Options
complete -c ghost -n '__fish_seen_subcommand_from init install uninstall' -l global -d 'Global installation'
complete -c ghost -n '__fish_seen_subcommand_from init install' -l dry-run -d 'Preview changes'
complete -c ghost -n '__fish_seen_subcommand_from init' -l yes -d 'Auto-install binaries if missing'
complete -c ghost -n '__fish_seen_subcommand_from init' -l interactive -d 'Guided setup wizard'
complete -c ghost -n '__fish_seen_subcommand_from doctor' -l fix -d 'Auto-fix issues where possible'
complete -c ghost -n '__fish_seen_subcommand_from audit' -l all -d 'Audit all commits'
complete -c ghost -n '__fish_seen_subcommand_from audit' -l range -d 'Commit range'
complete -c ghost -n '__fish_seen_subcommand_from audit' -l threshold -d 'Threshold'
complete -c ghost -n '__fish_seen_subcommand_from audit check blame stats' -l json -d 'JSON output'
)";
    } else {
        std::cerr << ghost::output::Style::error("Unsupported shell: " + shell) << "\n";
        std::cerr << ghost::output::Style::dim("Supported: bash, zsh, fish") << "\n";
        return GHOST_EXIT_ERROR;
    }
    return GHOST_EXIT_OK;
}

int main(int argc, char* argv[]) {
    // Check verbose first (global flag)
    // g_verbose will be set during command extraction below
    
    if (argc < 2) {
        ghost::cli::CommandRegistry::printGlobalHelp();
        return GHOST_EXIT_ERROR;
    }

    // Extract command, skipping global flags at argv[1]
    int cmdIndex = 1;
    while (cmdIndex < argc && std::string(argv[cmdIndex]).starts_with("-")) {
        if (std::string(argv[cmdIndex]) == "--verbose" || std::string(argv[cmdIndex]) == "-v") {
            g_verbose = true;
        }
        cmdIndex++;
    }
    
    std::string rawCommand;
    if (cmdIndex < argc) {
        rawCommand = argv[cmdIndex];
    }
    
    // Handle help and version specially
    if (rawCommand == "--help" || rawCommand == "-h" || rawCommand == "-?" || rawCommand == "help") {
        if (argc > 2) {
            std::string cmd = ghost::cli::CommandRegistry::resolveCommand(argv[2]);
            if (!cmd.empty()) {
                ghost::cli::CommandRegistry::printHelp(cmd);
                return GHOST_EXIT_OK;
            }
            std::cerr << ghost::output::Style::error("Unknown command: " + std::string(argv[2])) << "\n";
            printSuggestion(argv[2]);
            return GHOST_EXIT_ERROR;
        }
        ghost::cli::CommandRegistry::printGlobalHelp();
        return GHOST_EXIT_OK;
    }
    
    // Resolve command (with fuzzy matching)
    std::string command = ghost::cli::CommandRegistry::resolveCommand(rawCommand);
    
    if (command.empty()) {
        std::cerr << ghost::output::Style::error("Unknown command: " + rawCommand) << "\n";
        printSuggestion(rawCommand);
        return GHOST_EXIT_ERROR;
    }
    
    // Per-command --help
    if (hasFlag(argc, argv, "--help") || hasFlag(argc, argv, "-h") || hasFlag(argc, argv, "-?")) {
        ghost::cli::CommandRegistry::printHelp(command);
        return GHOST_EXIT_OK;
    }
    
    logVerbose("resolved command: " + command + " (from: " + rawCommand + ")");
    
    // Route to handlers
    if (command == "version") {
        ghost::cli::CommandRegistry::printVersion();
        return GHOST_EXIT_OK;
    } else if (command == "init") {
        return handleInit(argc, argv);
    } else if (command == "install") {
        return handleInstall(argc, argv);
    } else if (command == "uninstall") {
        return handleUninstall(argc, argv);
    } else if (command == "install-hooks") {
        return handleInstallHooks(argc, argv);
    } else if (command == "uninstall-hooks") {
        return handleUninstallHooks(argc, argv);
    } else if (command == "audit") {
        return handleAudit(argc, argv);
    } else if (command == "check") {
        return handleCheck(argc, argv);
    } else if (command == "blame") {
        return handleBlame(argc, argv);
    } else if (command == "show") {
        return handleShow(argc, argv);
    } else if (command == "stats") {
        return handleStats(argc, argv);
    } else if (command == "config") {
        return handleConfig(argc, argv);
    } else if (command == "doctor") {
        return handleDoctor(argc, argv);
    } else if (command == "status") {
        return handleStatus(argc, argv);
    } else if (command == "completion") {
        return handleCompletion(argc, argv);
    } else if (command == "post-commit") {
        return handlePostCommit(argc, argv);
    } else {
        std::cerr << ghost::output::Style::error("Command not yet implemented: " + command) << "\n";
        return GHOST_EXIT_ERROR;
    }
}
