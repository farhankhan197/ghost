#include <iostream>
#include <string>
#include <vector>
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
#include "persist/db.hpp"

namespace fs = std::filesystem;

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

static bool hasFlag(int argc, char* argv[], const std::string& flag) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == flag) return true;
    }
    return false;
}

static void ensureGhostDir(const std::string& repoRoot) {
    std::error_code ec;
    fs::create_directories(fs::path(repoRoot) / ".git" / "ghost", ec);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage: ghost-checkpoint <command> [options]\n";
        std::cout << "Commands: pre, post, show, reset\n";
        std::cout << "Options:\n";
        std::cout << "  --agent <name>     Agent name (required)\n";
        std::cout << "  --model <model>    Model name (optional)\n";
        std::cout << "  --file <path>      Target file for per-edit checkpoint (optional)\n";
        return 1;
    }

    std::string command = argv[1];
    std::string repoRoot = ghost::git::Repo::getRoot();

    if (repoRoot.empty()) {
        std::cerr << "Not in a git repository\n";
        return 1;
    }

    ensureGhostDir(repoRoot);
    auto* db = ghost::persist::getRepoDb(repoRoot);
    if (!db) {
        std::cerr << "Failed to open ghost database\n";
        return 1;
    }

    if (command == "pre") {
        std::string agent = getArg(argc, argv, "--agent");
        std::string targetFile = getArg(argc, argv, "--file");
        if (agent.empty()) {
            std::cerr << "Usage: ghost-checkpoint pre --agent <name> [--file <path>]\n";
            return 1;
        }

        std::cout << "Capturing snapshot for agent: " << agent << "\n";

        std::vector<std::string> files;
        if (!targetFile.empty()) {
            files.push_back(targetFile);
            // Snapshot just this file
            ghost::checkpoint::Snapshot::captureSingle(repoRoot, targetFile);
        } else {
            files = ghost::checkpoint::Snapshot::capture(repoRoot);
        }

        time_t now = std::time(nullptr);

        // Save to DB
        ghost::persist::Checkpoint cp;
        cp.agent = agent;
        cp.model = "unknown";
        cp.target_file = targetFile.empty() ? "*" : targetFile;
        cp.snapshot_path = (fs::path(repoRoot) / ".git" / "ghost" / "snapshot").string();
        cp.ts_start = now;
        cp.processed = false;
        db->saveCheckpoint(cp);

        // Also save legacy pre-state for backward compatibility
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
        std::string targetFile = getArg(argc, argv, "--file");
        if (agent.empty()) {
            std::cerr << "Usage: ghost-checkpoint post --agent <name> --model <model> [--file <path>]\n";
            return 1;
        }
        if (model.empty() || model == "unknown") {
            const char* home = std::getenv("USERPROFILE");
            if (!home) home = std::getenv("HOME");
            if (home) {
                std::string modelPath = std::string(home) + "/.ghost/.current_model";
                std::ifstream modelFile(modelPath);
                if (modelFile.is_open()) {
                    std::getline(modelFile, model);
                    while (!model.empty() && (model.back() == '\n' || model.back() == '\r')) {
                        model.pop_back();
                    }
                }
            }
        }

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

        // If --file was specified, only process that file
        std::vector<std::string> processFiles = preState.files;
        if (!targetFile.empty()) {
            processFiles = {targetFile};
        }

        for (const auto& file : processFiles) {
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

        ghost::checkpoint::Session::write(
            repoRoot, sessionId, agent, model, author,
            preState.ts_start, ts_end, entries, totalAdditions, totalDeletions
        );

        // Save session to DB as well
        std::ostringstream sessJson;
        sessJson << "{\"session_id\":\"" << sessionId << "\",";
        sessJson << "\"agent\":\"" << agent << "\",";
        sessJson << "\"model\":\"" << model << "\",";
        sessJson << "\"author\":\"" << author << "\",";
        sessJson << "\"ts_start\":" << preState.ts_start << ",";
        sessJson << "\"ts_end\":" << ts_end << ",";
        sessJson << "\"additions\":" << totalAdditions << ",";
        sessJson << "\"deletions\":" << totalDeletions << ",";
        sessJson << "\"entries\":[";
        for (size_t i = 0; i < entries.size(); ++i) {
            if (i > 0) sessJson << ",";
            sessJson << "{\"file_path\":\"" << entries[i].file_path << "\",";
            sessJson << "\"ranges\":\"" << entries[i].ranges << "\"}";
        }
        sessJson << "]}";

        ghost::persist::Session sess;
        sess.session_id = sessionId;
        sess.agent = agent;
        sess.model = model;
        sess.author = author;
        sess.ts_start = preState.ts_start;
        sess.ts_end = ts_end;
        sess.additions = totalAdditions;
        sess.deletions = totalDeletions;
        sess.json_data = sessJson.str();
        sess.committed = false;
        db->saveSession(sess);

        // Mark checkpoint as processed
        auto checkpoints = db->loadCheckpoints(true);
        for (const auto& cp : checkpoints) {
            if (targetFile.empty() || cp.target_file == targetFile || cp.target_file == "*") {
                db->markCheckpointProcessed(cp.id);
            }
        }

        ghost::checkpoint::WorkingLog::clearPreState(repoRoot);

        std::cout << "Session recorded: " << sessionId << "\n";
        std::cout << "  Agent: " << agent << "\n";
        std::cout << "  Model: " << model << "\n";
        std::cout << "  Files changed: " << entries.size() << "\n";
        std::cout << "  Additions: " << totalAdditions << "\n";
        std::cout << "  Deletions: " << totalDeletions << "\n";

    } else if (command == "show") {
        auto preState = ghost::checkpoint::WorkingLog::loadPreState(repoRoot);
        if (preState.valid) {
            std::cout << "Legacy active session:\n";
            std::cout << "  Agent: " << preState.agent << "\n";
            std::cout << "  Files: " << preState.files.size() << "\n";
            for (const auto& f : preState.files) {
                std::cout << "    " << f << "\n";
            }
        }

        auto checkpoints = db->loadCheckpoints(true);
        if (!checkpoints.empty()) {
            std::cout << "Active checkpoints (DB):\n";
            for (const auto& cp : checkpoints) {
                std::cout << "  " << cp.target_file << " (" << cp.agent << ")\n";
            }
        }

        auto sessions = db->loadSessions(true);
        if (!sessions.empty()) {
            std::cout << "Uncommitted sessions (DB):\n";
            for (const auto& s : sessions) {
                std::cout << "  " << s.session_id << " (" << s.agent << ", " << s.additions << " additions)\n";
            }
        }

        if (!preState.valid && checkpoints.empty() && sessions.empty()) {
            std::cout << "No active sessions or checkpoints\n";
        }

    } else if (command == "reset") {
        ghost::checkpoint::WorkingLog::clearPreState(repoRoot);
        db->clearCheckpoints();
        std::cout << "Pre-state cleared\n";

    } else {
        std::cerr << "Unknown command: " << command << "\n";
        return 1;
    }

    return 0;
}
