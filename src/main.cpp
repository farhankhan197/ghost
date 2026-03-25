#include <iostream>
#include <string>
#include "git/repo.hpp"
#include "git/notes.hpp"

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "ghost - Git Hook for Origin Source Tracking\n";
        std::cout << "Usage: ghost <command> [options]\n";
        std::cout << "\nCommands:\n";
        std::cout << "  install-hooks    Install agent hooks\n";
        std::cout << "  audit            Run audit on current PR\n";
        std::cout << "  blame            Show line attribution\n";
        std::cout << "  stats            Show AI% statistics\n";
        std::cout << "  show             Show ghost note for commit\n";
        std::cout << "  config           Show/set configuration\n";
        std::cout << "  version          Print version\n";
        return 1;
    }

    std::string command = argv[1];

    if (command == "version") {
        std::cout << "ghost version 1.0.0\n";
    } else if (command == "install-hooks") {
        std::cout << "Installing hooks...\n";
    } else if (command == "audit") {
        std::cout << "Running audit...\n";
    } else if (command == "blame") {
        std::cout << "Showing blame...\n";
    } else if (command == "stats") {
        std::cout << "Showing stats...\n";
    } else if (command == "show") {
        // Usage: ghost show <commit>
        if (argc < 3) {
            std::cerr << "Usage: ghost show <commit>\n";
            return 1;
        }
        std::string commit_sha = argv[2];
        std::string note = ghost::git::Notes::show("refs/notes/ghost", commit_sha);
        if (note.empty()) {
            std::cout << "No ghost note found for " << commit_sha << "\n";
        } else {
            std::cout << note << "\n";
        }
    } else if (command == "config") {
        std::cout << "Configuration...\n";
    } else {
        std::cerr << "Unknown command: " << command << "\n";
        return 1;
    }

    return 0;
}