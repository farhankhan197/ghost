#include <iostream>
#include <string>
#include "output/style.hpp"
#include "cli/audit_commands.hpp"
#include "cli/basic_commands.hpp"
#include "cli/commands.hpp"
#include "cli/doctor_command.hpp"
#include "cli/exit_codes.hpp"
#include "cli/governance_commands.hpp"
#include "cli/hook_commands.hpp"
#include "cli/inspection_commands.hpp"
#include "cli/notes_command.hpp"
#include "cli/post_commit_command.hpp"
#include "cli/rewrite_commands.hpp"
#include "cli/status_commands.hpp"

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

// Exit codes (avoid standard macro conflicts)
static constexpr int GHOST_EXIT_OK = ghost::cli::kExitOk;
static constexpr int GHOST_EXIT_ERROR = ghost::cli::kExitError;

int main(int argc, char* argv[]) {
    // Check verbose first (global flag)
    // g_verbose will be set during command extraction below
    
    if (argc < 2) {
        ghost::cli::CommandRegistry::printGlobalHelp();
        return GHOST_EXIT_ERROR;
    }

    // Handle --version/-v and --help/-h before flag-skipping loop (only when no command follows)
    if (argc == 2) {
        std::string first = argv[1];
        if (first == "--version" || first == "-v") {
            ghost::cli::CommandRegistry::printVersion();
            return GHOST_EXIT_OK;
        }
        if (first == "--help" || first == "-h" || first == "-?" || first == "help") {
            ghost::cli::CommandRegistry::printGlobalHelp();
            return GHOST_EXIT_OK;
        }
    }
    if (argc == 3) {
        std::string first = argv[1];
        std::string second = argv[2];
        if (first == "--help" || first == "-h" || first == "-?" || first == "help") {
            std::string cmd = ghost::cli::CommandRegistry::resolveCommand(second);
            if (!cmd.empty()) {
                ghost::cli::CommandRegistry::printHelp(cmd);
                return GHOST_EXIT_OK;
            }
            std::cerr << ghost::output::Style::error("Unknown command: " + second) << "\n";
            ghost::cli::printSuggestion(second);
            return GHOST_EXIT_ERROR;
        }
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
    
    // Resolve command (with fuzzy matching)
    std::string command = ghost::cli::CommandRegistry::resolveCommand(rawCommand);
    
    if (command.empty()) {
        std::cerr << ghost::output::Style::error("Unknown command: " + rawCommand) << "\n";
        ghost::cli::printSuggestion(rawCommand);
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
        return ghost::cli::init(argc, argv, g_verbose);
    } else if (command == "uninstall") {
        return ghost::cli::uninstall(argc, argv);
    } else if (command == "install-hooks") {
        return ghost::cli::installHooks(argc, argv, g_verbose);
    } else if (command == "uninstall-hooks") {
        return ghost::cli::uninstallHooks(argc, argv, g_verbose);
    } else if (command == "audit") {
        return ghost::cli::audit(argc, argv, g_verbose);
    } else if (command == "verify-pr") {
        return ghost::cli::verifyPr(argc, argv, g_verbose);
    } else if (command == "check") {
        return ghost::cli::check(argc, argv, g_verbose);
    } else if (command == "blame") {
        return ghost::cli::blame(argc, argv, g_verbose);
    } else if (command == "show") {
        return ghost::cli::show(argc, argv, g_verbose);
    } else if (command == "stats") {
        return ghost::cli::stats(argc, argv, g_verbose);
    } else if (command == "config") {
        return ghost::cli::config(argc, argv);
    } else if (command == "policy") {
        return ghost::cli::policy(argc, argv);
    } else if (command == "notes") {
        return ghost::cli::notes(argc, argv);
    } else if (command == "banish") {
        return ghost::cli::banish(argc, argv);
    } else if (command == "doctor") {
        return ghost::cli::doctor(argc, argv);
    } else if (command == "status") {
        return ghost::cli::status(argc, argv, g_verbose);
    } else if (command == "explain") {
        return ghost::cli::explain(argc, argv);
    } else if (command == "completion") {
        return ghost::cli::completion(argc, argv, g_verbose);
    } else if (command == "post-commit") {
        return ghost::cli::postCommit(argc, argv, g_verbose);
    } else if (command == "rewrite-log") {
        return ghost::cli::rewriteLog(argc, argv);
    } else if (command == "working-state") {
        return ghost::cli::workingState(argc, argv);
    } else {
        std::cerr << ghost::output::Style::error("Command not yet implemented: " + command) << "\n";
        return GHOST_EXIT_ERROR;
    }
}
