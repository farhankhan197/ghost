#include <iostream>
#include <string>
#include <vector>
#include <set>
#include <ctime>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <sstream>
#include <fstream>
#include <filesystem>
#include "working_log.hpp"
#include "snapshot.hpp"
#include "session.hpp"
#include "repo.hpp"

static std::string runCommand(const std::string& cmd) {
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) return "";
    std::string result;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe.get())) result += buffer;
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) result.pop_back();
    return result;
}

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
        std::cout << "Usage: ghost-checkpoint <command> [options]\n";
        std::cout << "Commands: pre, post, show, reset\n";
        return 1;
    }

    std::string command = argv[1];
    std::string repoRoot = ghost::git::Repo::getRoot();

    if (repoRoot.empty()) {
        std::cerr << "Not in a git repository\n";
        return 1;
    }

    if (command == "pre") {
        std::string agent = getArg(argc, argv, "--agent");
        if (agent.empty()) {
            std::cerr << "Usage: ghost-checkpoint pre --agent <name>\n";
            return 1;
        }

        std::cout << "Capturing snapshot for agent: " << agent << "\n";

        std::vector<std::string> files = ghost::checkpoint::Snapshot::capture(repoRoot);

        time_t now = std::time(nullptr);
        ghost::checkpoint::WorkingLog::savePreState(repoRoot, agent, now, files);

        if (files.empty()) {
            std::cout << "Pre-state saved (no modified files)\n";
        } else {
            std::cout << "Snapshot captured: " << files.size() << " file(s)\n";
            for (const auto& f : files) {
                std::cout << "  " << f << "\n";
            }
        }

    } else if (command == "post") {
        std::string agent = getArg(argc, argv, "--agent");
        std::string model = getArg(argc, argv, "--model");
        if (agent.empty()) {
            std::cerr << "Usage: ghost-checkpoint post --agent <name> --model <model>\n";
            return 1;
        }
        if (model.empty()) model = "unknown";

        auto preState = ghost::checkpoint::WorkingLog::loadPreState(repoRoot);
        if (!preState.valid) {
            std::cerr << "No pre-state found. Run 'ghost-checkpoint pre' first.\n";
            return 1;
        }

        std::string ghostDir = ghost::checkpoint::WorkingLog::getGhostDir(repoRoot);
        std::string sessionId = ghost::checkpoint::Session::generateId();
        std::string author = ghost::checkpoint::Session::getGitAuthor(repoRoot);
        time_t ts_end = std::time(nullptr);

        std::vector<ghost::checkpoint::SessionEntry> entries;
        int totalAdditions = 0;
        int totalDeletions = 0;

        std::string snapshotDir = ghostDir + "/snapshot";

        std::set<std::string> snapshotFiles(preState.files.begin(), preState.files.end());

        for (const auto& file : preState.files) {
            std::string snapshotPath = snapshotDir + "/" + file;
            std::string currentPath = repoRoot + "/" + file;

            auto changes = ghost::checkpoint::Session::computeChanges(snapshotPath, currentPath, file);

            if (!changes.added_ranges.empty() || changes.deletions > 0) {
                ghost::checkpoint::SessionEntry entry;
                entry.file_path = changes.file_path;
                entry.ranges = changes.added_ranges.toString();
                entries.push_back(entry);
                totalAdditions += changes.additions;
                totalDeletions += changes.deletions;
            }
        }

        std::string currentDiff = runCommand("git diff --name-only");
        std::istringstream currentStream(currentDiff);
        std::string line;
        while (std::getline(currentStream, line)) {
            if (line.empty() || snapshotFiles.count(line)) continue;

            std::string headPath = repoRoot + "/.git/ghost/head_copy/" + line;
            std::filesystem::create_directories(std::filesystem::path(headPath).parent_path());

            std::string headContent = runCommand("git show HEAD:\"" + line + "\" 2>nul");
            if (!headContent.empty()) {
                std::ofstream headFile(headPath);
                headFile << headContent;
                headFile.close();

                auto changes = ghost::checkpoint::Session::computeChanges(headPath, repoRoot + "/" + line, line);
                std::filesystem::remove(headPath);

                if (!changes.added_ranges.empty() || changes.deletions > 0) {
                    ghost::checkpoint::SessionEntry entry;
                    entry.file_path = changes.file_path;
                    entry.ranges = changes.added_ranges.toString();
                    entries.push_back(entry);
                    totalAdditions += changes.additions;
                    totalDeletions += changes.deletions;
                }
            } else {
                std::ifstream newFile(repoRoot + "/" + line);
                if (newFile.is_open()) {
                    std::string content((std::istreambuf_iterator<char>(newFile)), std::istreambuf_iterator<char>());
                    int lineCount = 0;
                    for (char c : content) if (c == '\n') lineCount++;
                    if (!content.empty() && content.back() != '\n') lineCount++;

                    ghost::checkpoint::SessionEntry entry;
                    entry.file_path = line;
                    entry.ranges = lineCount > 0 ? "1-" + std::to_string(lineCount) : "1";
                    entries.push_back(entry);
                    totalAdditions += lineCount > 0 ? lineCount : 1;
                }
            }
        }

        ghost::checkpoint::Session::write(
            repoRoot, sessionId, agent, model, author,
            preState.ts_start, ts_end, entries, totalAdditions, totalDeletions
        );

        ghost::checkpoint::WorkingLog::clearPreState(repoRoot);

        std::cout << "Session recorded: " << sessionId << "\n";
        std::cout << "  Agent: " << agent << "\n";
        std::cout << "  Model: " << model << "\n";
        std::cout << "  Files changed: " << entries.size() << "\n";
        std::cout << "  Additions: " << totalAdditions << "\n";
        std::cout << "  Deletions: " << totalDeletions << "\n";

    } else if (command == "show") {
        auto preState = ghost::checkpoint::WorkingLog::loadPreState(repoRoot);
        if (!preState.valid) {
            std::cout << "No active session\n";
            return 0;
        }

        std::cout << "Active session:\n";
        std::cout << "  Agent: " << preState.agent << "\n";
        std::cout << "  Started: " << preState.ts_start << "\n";
        std::cout << "  Files: " << preState.files.size() << "\n";
        for (const auto& f : preState.files) {
            std::cout << "    " << f << "\n";
        }

        auto sessions = ghost::checkpoint::WorkingLog::listSessions(repoRoot);
        if (!sessions.empty()) {
            std::cout << "Completed sessions:\n";
            for (const auto& s : sessions) {
                std::cout << "  " << s << "\n";
            }
        }

    } else if (command == "reset") {
        ghost::checkpoint::WorkingLog::clearPreState(repoRoot);
        std::cout << "Pre-state cleared\n";

    } else {
        std::cerr << "Unknown command: " << command << "\n";
        return 1;
    }

    return 0;
}
