#include <iostream>
#include <string>
#include "git/repo.hpp"
#include "git/notes.hpp"
#include "note/reader.hpp"
#include "commit/post_commit.hpp"
#include "hooks/installer.hpp"

static std::string getArg(int argc, char* argv[], const std::string& flag) {
    for (int i = 0; i < argc - 1; ++i) {
        if (std::string(argv[i]) == flag && i + 1 < argc) {
            return argv[i + 1];
        }
    }
    return "";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "ghost - Git Hook for Origin Source Tracking\n";
        std::cout << "Usage: ghost <command> [options]\n";
        std::cout << "\nCommands:\n";
        std::cout << "  install              Install ghost in current repo\n";
        std::cout << "  install --global     Install ghost for all repos (opencode)\n";
        std::cout << "  install-bin          Copy binaries to ~/.ghost/bin\n";
        std::cout << "  uninstall            Remove ghost from current repo\n";
        std::cout << "  uninstall --global   Remove global ghost plugin\n";
        std::cout << "  show <commit>        Show ghost note for commit\n";
        std::cout << "  post-commit          Run post-commit hook\n";
        std::cout << "  version              Print version\n";
        return 1;
    }

    std::string command = argv[1];

    if (command == "version") {
        std::cout << "ghost version 1.0.0\n";
    } else if (command == "install") {
        if (argc > 2 && std::string(argv[2]) == "--global") {
            return ghost::hooks::Installer::installGlobal();
        }
        if (argc > 2 && std::string(argv[2]) == "-bin") {
            return ghost::hooks::Installer::installBin();
        }
        std::string repoRoot = ghost::git::Repo::getRoot();
        if (repoRoot.empty()) {
            std::cerr << "Not in a git repository\n";
            return 1;
        }
        int binResult = ghost::hooks::Installer::installBin();
        if (binResult != 0) {
            std::cerr << "Warning: failed to install binaries, plugin may not work\n";
        }
        return ghost::hooks::Installer::installRepo(repoRoot);
    } else if (command == "uninstall") {
        if (argc > 2 && std::string(argv[2]) == "--global") {
            return ghost::hooks::Installer::uninstallGlobal();
        }
        std::string repoRoot = ghost::git::Repo::getRoot();
        if (repoRoot.empty()) {
            std::cerr << "Not in a git repository\n";
            return 1;
        }
        return ghost::hooks::Installer::uninstallRepo(repoRoot);
    } else if (command == "install-bin") {
        return ghost::hooks::Installer::installBin();
    } else if (command == "show") {
        if (argc < 3) {
            std::cerr << "Usage: ghost show <commit>\n";
            return 1;
        }
        std::string commit_sha = argv[2];
        std::string note = ghost::git::Notes::show("refs/notes/ghost", commit_sha);
        if (note.empty()) {
            std::cout << "No ghost note found for " << commit_sha << "\n";
        } else {
            auto result = ghost::note::NoteReader::parse(note);
            if (!result.success) {
                std::cout << "Failed to parse note: " << result.error << "\n";
                std::cout << "\nRaw note:\n" << note << "\n";
            } else {
                for (const auto& entry : result.entries) {
                    std::cout << entry.file_path << "\n";
                    auto it = result.sessions.find(entry.session_id);
                    if (it != result.sessions.end()) {
                        const auto& sess = it->second;
                        std::cout << "  " << entry.session_id
                                  << "  lines " << entry.ranges.toString()
                                  << "  (" << sess.agent << " / " << sess.model << ")\n";
                    } else {
                        std::cout << "  " << entry.session_id
                                  << "  lines " << entry.ranges.toString() << "\n";
                    }
                }
            }
        }
    } else if (command == "post-commit") {
        std::string repoRoot = ghost::git::Repo::getRoot();
        std::string commitSha = ghost::git::Repo::getHead();
        if (repoRoot.empty() || commitSha.empty()) {
            std::cerr << "Not in a git repository\n";
            return 1;
        }
        return ghost::commit::PostCommit::run(repoRoot, commitSha);
    } else {
        std::cerr << "Unknown command: " << command << "\n";
        return 1;
    }

    return 0;
}
