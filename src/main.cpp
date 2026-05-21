#include <iostream>
#include <string>
#include "git/repo.hpp"
#include "git/notes.hpp"
#include "git/blame.hpp"
#include "note/reader.hpp"
#include "commit/post_commit.hpp"
#include "hooks/installer.hpp"
#include "hooks/agent_hooks.hpp"
#include "hooks/agent_detector.hpp"
#include "audit/auditor.hpp"
#include "audit/blame_overlay.hpp"
#include "output/report.hpp"
#include "output/style.hpp"
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
    
    opts="install uninstall audit check blame show stats config install-hooks uninstall-hooks completion version help --global --dry-run --json --all --range --threshold --verbose"
    
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
complete -c ghost -n '__fish_use_subcommand' -a completion -d 'Generate shell completion script'
complete -c ghost -n '__fish_use_subcommand' -a version -d 'Print version information'
complete -c ghost -n '__fish_use_subcommand' -a help -d 'Show help'

# Options
complete -c ghost -n '__fish_seen_subcommand_from install uninstall' -l global -d 'Global installation'
complete -c ghost -n '__fish_seen_subcommand_from install' -l dry-run -d 'Preview changes'
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
        // check is same as audit but for staged
        return handleAudit(argc, argv);
    } else if (command == "blame") {
        return handleBlame(argc, argv);
    } else if (command == "show") {
        return handleShow(argc, argv);
    } else if (command == "stats") {
        return handleStats(argc, argv);
    } else if (command == "config") {
        return handleConfig(argc, argv);
    } else if (command == "completion") {
        return handleCompletion(argc, argv);
    } else if (command == "post-commit") {
        return handlePostCommit(argc, argv);
    } else {
        std::cerr << ghost::output::Style::error("Command not yet implemented: " + command) << "\n";
        return GHOST_EXIT_ERROR;
    }
}
