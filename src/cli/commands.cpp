#include "commands.hpp"
#include "../output/style.hpp"
#include <iostream>
#include <algorithm>
#include <cmath>

namespace ghost {
namespace cli {

// Levenshtein distance for fuzzy matching
static int levenshtein(const std::string& s1, const std::string& s2) {
    int m = s1.length(), n = s2.length();
    if (m == 0) return n;
    if (n == 0) return m;
    
    std::vector<std::vector<int>> dp(m + 1, std::vector<int>(n + 1));
    for (int i = 0; i <= m; ++i) dp[i][0] = i;
    for (int j = 0; j <= n; ++j) dp[0][j] = j;
    
    for (int i = 1; i <= m; ++i) {
        for (int j = 1; j <= n; ++j) {
            int cost = (s1[i-1] == s2[j-1]) ? 0 : 1;
            dp[i][j] = std::min({dp[i-1][j] + 1, dp[i][j-1] + 1, dp[i-1][j-1] + cost});
        }
    }
    return dp[m][n];
}

static double similarity(const std::string& s1, const std::string& s2) {
    int maxLen = std::max(s1.length(), s2.length());
    if (maxLen == 0) return 1.0;
    return 1.0 - (double)levenshtein(s1, s2) / maxLen;
}

static const std::map<std::string, CommandInfo> COMMANDS = {
    {"init", {
        "init", {},
        "Initialize ghost in a git repo (config + hooks)",
        "ghost init [--owner|--contributor] [--mode <mode>] [--github-owner <owner>] [--yes] [--interactive] [--global] [--dry-run]",
        {
            "ghost init                 Scaffold config and hooks in current repo",
            "ghost init --owner         Maintainer setup: restrictive policy, CI workflow, GHOST.md",
            "ghost init --contributor   Contributor setup: local hooks and notes refs only",
            "ghost init --owner --mode locked",
            "ghost init --owner --github-owner @org/team",
            "ghost init --yes           One-shot: config + hooks + binaries",
            "ghost init --global        Install plugin for all repos (no repo needed)",
            "ghost init --interactive   Guided setup wizard",
            "ghost init --dry-run         Preview what would be configured"
        },
        {"--owner", "--contributor", "--mode", "--github-owner", "--force", "--yes", "-y", "--interactive", "-i", "--global", "-g", "--dry-run", "-n"}
    }},
    {"install", {
        "install", {"i", "in"},
        "[DEPRECATED] Use 'ghost init --yes' instead",
        "ghost install [--global]",
        {
            "ghost install              Redirects to 'ghost init --yes'",
            "ghost install --global     Install global plugin for all repos"
        },
        {"--global", "-g"}
    }},
    {"uninstall", {
        "uninstall", {"u", "un"},
        "Remove ghost from the current repository",
        "ghost uninstall [--global]",
        {
            "ghost uninstall            Remove from current repo",
            "ghost uninstall --global   Remove global plugin"
        },
        {"--global", "-g"}
    }},
    {"install-hooks", {
        "install-hooks", {"ih"},
        "Auto-configure AI agent hooks",
        "ghost install-hooks [--repo] [--agent <name>]",
        {
            "ghost install-hooks        Install for all detected agents",
            "ghost install-hooks --agent opencode   Install for specific agent"
        },
        {"--repo", "--agent", "-a"}
    }},
    {"uninstall-hooks", {
        "uninstall-hooks", {"uh"},
        "Remove AI agent hooks",
        "ghost uninstall-hooks [--repo] [--agent <name>]",
        {},
        {"--repo", "--agent", "-a"}
    }},
    {"audit", {
        "audit", {"a", "aud"},
        "Enforce policy against committed history using ghost/git-ai notes",
        "ghost audit [<commit>] [--all] [--range <range>] [--threshold <n>] [--config-ref <ref>] [--json]",
        {
            "ghost audit                Audit HEAD commit/codebase after commit",
            "ghost audit abc123         Audit specific committed revision",
            "ghost audit --all          Audit all commits with ghost notes",
            "ghost audit --range HEAD~10..HEAD",
            "ghost audit --threshold 50 --json",
            "ghost audit --config-ref origin/main   Use owner policy from base branch",
            "ghost check                Check staged changes before commit"
        },
        {"--all", "--range", "--threshold", "--config-ref", "--json", "-a", "-r", "-t", "-j"}
    }},
    {"verify-pr", {
        "verify-pr", {"vp"},
        "Simulate the PR audit locally using base-branch owner policy",
        "ghost verify-pr [<range>] [--base <ref>] [--json] [--no-fetch]",
        {
            "ghost verify-pr                 Verify origin/main..HEAD with origin/main policy",
            "ghost verify-pr origin/main..HEAD",
            "ghost verify-pr --base origin/develop",
            "ghost verify-pr --no-fetch",
            "ghost verify-pr --json"
        },
        {"--base", "--json", "-j", "--no-fetch"}
    }},
    {"banish", {
        "banish", {"b", "ban"},
        "Banish files from ghost AI tracking (owner only)",
        "ghost banish <path> [<path> ...] [--list] [--clear]",
        {
            "ghost banish src/foo.cpp          Banish a file from AI tracking",
            "ghost banish build/ dist/          Banish directories",
            "ghost banish --list                Show currently banished paths",
            "ghost banish --clear src/foo.cpp   Remove a file from banish list",
            "ghost banish --clear               Clear all banished paths"
        },
        {"--list", "--clear"}
    }},
    {"check", {
        "check", {"c", "chk"},
        "Preview attribution for staged changes before commit",
        "ghost check [--json]",
        {
            "ghost check                Check only the staged diff before commit",
            "ghost check --json         Output JSON"
        },
        {"--json", "-j"}
    }},
    {"blame", {
        "blame", {"b", "bl"},
        "Line-by-line attribution for a file",
        "ghost blame <file> [--json]",
        {
            "ghost blame src/main.cpp   Show attribution per line",
            "ghost blame src/main.cpp --json"
        },
        {"--json", "-j"}
    }},
    {"show", {
        "show", {"s", "sh"},
        "Show raw ghost note for a commit",
        "ghost show <commit>",
        {
            "ghost show HEAD            Show ghost note for HEAD",
            "ghost show abc123          Show ghost note for commit"
        },
        {}
    }},
    {"stats", {
        "stats", {"st"},
        "AI% statistics for HEAD or a range",
        "ghost stats [<range>] [--json]",
        {
            "ghost stats                Stats for HEAD~1..HEAD",
            "ghost stats HEAD~5..HEAD   Stats for last 5 commits",
            "ghost stats --json         Output JSON"
        },
        {"--json", "-j"}
    }},
    {"config", {
        "config", {"cfg"},
        "Show or set ghost.yml configuration (owner-gated policy keys)",
        "ghost config [set <key> <value>]",
        {
            "ghost config               Show current config",
            "ghost config set threshold 50",
            "ghost config set required true",
            "ghost config set owner maintainer@example.com"
        },
        {"set"}
    }},
    {"policy", {
        "policy", {"pol"},
        "Show, sign, and manage repo owner policy",
        "ghost policy [set mode <mode>|lock|unlock --force|sign|verify]",
        {
            "ghost policy                    Show owner, protected rules, and enforcement stages",
            "ghost policy set mode restrictive",
            "ghost policy set mode locked",
            "ghost policy lock",
            "ghost policy unlock --force",
            "ghost policy sign",
            "ghost policy verify"
        },
        {"set", "mode", "lock", "unlock", "sign", "verify", "--force"}
    }},
    {"notes", {
        "notes", {"note"},
        "Sign or verify Ghost attribution notes",
        "ghost notes <sign|verify> [commit] [--range <range>]",
        {
            "ghost notes sign                 Sign HEAD ghost notes",
            "ghost notes verify               Verify HEAD ghost notes",
            "ghost notes sign abc123",
            "ghost notes verify abc123",
            "ghost notes verify --range origin/main..HEAD"
        },
        {"sign", "verify", "--range"}
    }},
    {"doctor", {
        "doctor", {"doc", "dr"},
        "Diagnose ghost setup and suggest fixes",
        "ghost doctor [--fix]",
        {
            "ghost doctor               Check setup health",
            "ghost doctor --fix         Auto-fix issues where possible"
        },
        {"--fix", "-f"}
    }},
    {"status", {
        "status", {"st"},
        "Show current repo setup, staged work, sessions, and HEAD notes",
        "ghost status",
        {
            "ghost status               Show what exists now; does not enforce committed history"
        },
        {}
    }},
    {"explain", {
        "explain", {"why", "exp"},
        "Explain what a ghost command reads and enforces",
        "ghost explain <topic>",
        {
            "ghost explain status       Explain current-state inspection",
            "ghost explain check        Explain staged pre-commit preview",
            "ghost explain audit        Explain committed enforcement",
            "ghost explain verify-pr    Explain local PR simulation",
            "ghost explain policy       Explain owner controls"
        },
        {"init", "status", "check", "audit", "verify-pr", "policy"}
    }},
    {"completion", {
        "completion", {"comp"},
        "Generate shell completion script",
        "ghost completion <shell>",
        {
            "ghost completion bash      Generate bash completions",
            "ghost completion zsh       Generate zsh completions",
            "ghost completion fish      Generate fish completions"
        },
        {}
    }},
    {"version", {
        "version", {"v", "ver", "--version", "-v"},
        "Print version information",
        "ghost version",
        {},
        {}
    }},
    {"help", {
        "help", {"h", "--help", "-h", "-?"},
        "Show help information",
        "ghost help [command]",
        {
            "ghost help                 Show this help",
            "ghost help install         Show help for install command"
        },
        {}
    }},
    {"post-commit", {
        "post-commit", {"pc"},
        "Run post-commit hook (internal use)",
        "ghost post-commit",
        {},
        {}
    }},
    {"rewrite-log", {
        "rewrite-log", {"rl"},
        "Log and process git rewrite events (internal/hook use)",
        "ghost rewrite-log [--stdin] [--event <type>] [--repo <path>]",
        {
            "ghost rewrite-log --stdin              Read stdin from post-rewrite hook",
            "ghost rewrite-log --event checkout     Log a checkout event"
        },
        {"--stdin", "--event", "--repo", "--prev", "--new"}
    }},
    {"working-state", {
        "working-state", {"ws"},
        "Save/restore working checkpoint state across git operations",
        "ghost working-state [--save|--restore|--clear] [--key <name>] [--repo <path>]",
        {
            "ghost working-state --save             Save current sessions",
            "ghost working-state --restore          Restore saved sessions",
            "ghost working-state --clear            Clear saved state"
        },
        {"--save", "--restore", "--clear", "--key", "--repo"}
    }}
};

const std::map<std::string, CommandInfo>& CommandRegistry::getCommands() {
    return COMMANDS;
}

std::vector<std::string> CommandRegistry::getAllCommands() {
    std::vector<std::string> result;
    for (const auto& [name, info] : COMMANDS) {
        result.push_back(name);
    }
    return result;
}

std::string CommandRegistry::resolveCommand(const std::string& input) {
    if (input.empty()) return "";

    // Exact match
    if (COMMANDS.count(input)) return input;
    
    // Check aliases
    for (const auto& [name, info] : COMMANDS) {
        for (const auto& alias : info.aliases) {
            if (alias == input) return name;
        }
    }
    
    // Fuzzy prefix match (e.g., "ins" -> "install")
    for (const auto& [name, info] : COMMANDS) {
        if (name.find(input) == 0) return name;
        for (const auto& alias : info.aliases) {
            if (alias.find(input) == 0) return name;
        }
    }
    
    return "";
}

std::vector<std::string> CommandRegistry::getSuggestions(const std::string& input, int max) {
    std::vector<std::pair<std::string, double>> scores;
    
    for (const auto& [name, info] : COMMANDS) {
        double score = similarity(input, name);
        for (const auto& alias : info.aliases) {
            score = std::max(score, similarity(input, alias));
        }
        if (score > 0.3) {
            scores.push_back({name, score});
        }
    }
    
    std::sort(scores.begin(), scores.end(), [](auto& a, auto& b) {
        return a.second > b.second;
    });
    
    std::vector<std::string> result;
    for (int i = 0; i < std::min(max, (int)scores.size()); ++i) {
        result.push_back(scores[i].first);
    }
    return result;
}

void CommandRegistry::printHelp(const std::string& command) {
    auto it = COMMANDS.find(command);
    if (it == COMMANDS.end()) {
        std::cerr << "Unknown command: " << command << "\n";
        return;
    }
    
    const auto& info = it->second;
    using namespace ghost::output;
    
    std::cout << "\n" << Style::header("Ghost — " + info.name);
    std::cout << "  " << Style::dim(info.description) << "\n\n";
    
    std::cout << Style::bold(Style::blue("Usage:")) << "\n";
    std::cout << "  " << Style::glow(info.usage) << "\n\n";
    
    if (!info.aliases.empty()) {
        std::cout << Style::bold(Style::blue("Aliases:")) << "\n";
        std::cout << "  ";
        for (size_t i = 0; i < info.aliases.size(); ++i) {
            if (i > 0) std::cout << ", ";
            std::cout << Style::violet(info.aliases[i]);
        }
        std::cout << "\n\n";
    }
    
    if (!info.examples.empty()) {
        std::cout << Style::bold(Style::blue("Examples:")) << "\n";
        for (const auto& ex : info.examples) {
            std::cout << "  " << Style::dim("$") << " " << Style::glow(ex) << "\n";
        }
        std::cout << "\n";
    }
    
    if (!info.flags.empty()) {
        std::cout << Style::bold(Style::blue("Flags:")) << "\n";
        for (const auto& flag : info.flags) {
            std::cout << "  " << Style::violet(flag) << "\n";
        }
        std::cout << "\n";
    }
    
    std::cout << Style::dim("  Run 'ghost help' for all commands.\n\n");
}

void CommandRegistry::printGlobalHelp() {
    using namespace ghost::output;
    
    std::cout << Style::header("Ghost — Origin Source Tracking");
    std::cout << Style::dim("  Mandate code provenance. Recorded at the moment of creation.\n\n");
    
    std::cout << Style::bold(Style::blue("Usage:")) << " ghost " << Style::violet("<command>") << " " << Style::dim("[options]") << "\n\n";
    
    // Group commands
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> groups = {
        {"Setup", {}},
        {"Inspection", {}},
        {"Utility", {}}
    };
    
    for (const auto& [name, info] : COMMANDS) {
        if (name == "init" || name == "install" || name == "uninstall" || name == "install-hooks" || name == "uninstall-hooks") {
            groups["Setup"].push_back({name, info.description});
        } else if (name == "audit" || name == "check" || name == "blame" || name == "show" || name == "stats") {
            groups["Inspection"].push_back({name, info.description});
        } else if (name == "post-commit" || name == "rewrite-log" || name == "working-state") {
            groups["Internal"].push_back({name, info.description});
        } else {
            groups["Utility"].push_back({name, info.description});
        }
    }
    
    for (const auto& [group, commands] : groups) {
        std::cout << Style::bold(Style::blue("  " + group + ":\n"));
        for (const auto& [name, desc] : commands) {
            std::cout << "    " << Style::padRight(Style::violet(name), 18) << Style::dim(desc) << "\n";
        }
        std::cout << "\n";
    }
    
    std::cout << Style::dim("  Run 'ghost help <command>' for detailed help.\n");
    std::cout << Style::dim("  Run 'ghost <command> --help' for flag details.\n\n");
}

void CommandRegistry::printVersion() {
    using namespace ghost::output;
    std::cout << Style::header("Ghost " GHOST_VERSION);
    std::cout << Style::dim("  Commit attribution for the futuristic developer.\n\n");
}

} // namespace cli
} // namespace ghost
