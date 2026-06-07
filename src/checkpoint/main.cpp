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
#include <algorithm>
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

static std::string readStdinAll() {
    std::ostringstream ss;
    ss << std::cin.rdbuf();
    return ss.str();
}

static std::string extractJsonString(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos += search.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n')) {
        pos++;
    }
    if (pos >= json.size() || json[pos] != '"') return "";
    pos++;

    std::string result;
    bool escaped = false;
    for (; pos < json.size(); ++pos) {
        char c = json[pos];
        if (escaped) {
            switch (c) {
                case 'n': result += '\n'; break;
                case 'r': result += '\r'; break;
                case 't': result += '\t'; break;
                default: result += c; break;
            }
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '"') break;
        result += c;
    }
    return result;
}

static std::string extractCodexHookFile(const std::string& hookJson) {
    for (const auto& key : {"file_path", "filePath", "path", "file"}) {
        std::string value = extractJsonString(hookJson, key);
        if (!value.empty()) return value;
    }
    return "";
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
        std::cout << "  --codex-hook       Read Codex hook JSON from stdin (optional)\n";
        return 1;
    }

    std::string command = argv[1];
    std::string targetFile = getArg(argc, argv, "--file");
    bool codexHook = hasFlag(argc, argv, "--codex-hook");
    std::string codexHookJson = codexHook ? readStdinAll() : "";
    if (targetFile.empty() && !codexHookJson.empty()) {
        targetFile = extractCodexHookFile(codexHookJson);
    }
    std::string repoRoot = ghost::git::Repo::getRoot();

    if (repoRoot.empty()) {
        std::cerr << "Not in a git repository\n";
        return 1;
    }

    // If --file is absolute, resolve repo root from the file's repo
    // so checkpoint data lands in the correct repo even when CWD is elsewhere.
    if (!targetFile.empty()) {
        fs::path p(targetFile);
        if (p.is_absolute()) {
            fs::path dir = p.parent_path();
            std::error_code ec;
            while (dir.has_parent_path()) {
                if (fs::exists(dir / ".git", ec)) {
                    repoRoot = dir.string();
                    break;
                }
                dir = dir.parent_path();
            }
        }
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
        if (targetFile.empty() && !codexHookJson.empty()) {
            targetFile = extractCodexHookFile(codexHookJson);
        }
        if (agent.empty()) {
            std::cerr << "Usage: ghost-checkpoint pre --agent <name> [--file <path>]\n";
            return 1;
        }

        // Normalize absolute path to relative
        if (!targetFile.empty()) {
            fs::path p(targetFile);
            if (p.is_absolute()) {
                std::error_code ec;
                fs::path rel = fs::relative(p, repoRoot, ec);
                if (!ec) {
                    targetFile = rel.string();
                    for (char& c : targetFile) if (c == '\\') c = '/';
                }
            }
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
        if (targetFile.empty() && !codexHookJson.empty()) {
            targetFile = extractCodexHookFile(codexHookJson);
        }
        if (agent.empty()) {
            std::cerr << "Usage: ghost-checkpoint post --agent <name> --model <model> [--file <path>]\n";
            return 1;
        }

        // Normalize absolute path to relative
        if (!targetFile.empty()) {
            fs::path p(targetFile);
            if (p.is_absolute()) {
                std::error_code ec;
                fs::path rel = fs::relative(p, repoRoot, ec);
                if (!ec) {
                    targetFile = rel.string();
                    for (char& c : targetFile) if (c == '\\') c = '/';
                }
            }
        }
        if ((model.empty() || model == "unknown") && !codexHookJson.empty()) {
            std::string hookModel = extractJsonString(codexHookJson, "model");
            if (!hookModel.empty()) {
                size_t slash = hookModel.rfind('/');
                model = (slash == std::string::npos) ? hookModel : hookModel.substr(slash + 1);
            }
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
        if (model.empty() || model == "unknown") {
            model = agent;
        }

        auto preState = ghost::checkpoint::WorkingLog::loadPreState(repoRoot);
        time_t ts_start;
        std::vector<std::string> processFiles;

        if (!preState.valid) {
            // Standalone mode: no pre-state, use git diff HEAD to compute changes
            ts_start = std::time(nullptr);
            std::string changedOutput = runCommand("git diff --name-only HEAD --diff-filter=ACMR -- \"*\"");
            if (!changedOutput.empty()) {
                std::istringstream stream(changedOutput);
                std::string line;
                while (std::getline(stream, line)) {
                    if (!line.empty()) processFiles.push_back(line);
                }
            }
            std::string untrackedOutput = runCommand("git ls-files --others --exclude-standard");
            if (!untrackedOutput.empty()) {
                std::istringstream stream(untrackedOutput);
                std::string line;
                while (std::getline(stream, line)) {
                    if (!line.empty()) processFiles.push_back(line);
                }
            }
            if (!targetFile.empty()) {
                processFiles = {targetFile};
            }
        } else {
            ts_start = preState.ts_start;
            processFiles = preState.files;
            if (!targetFile.empty()) {
                processFiles = {targetFile};
            } else {
                std::string changedOutput = runCommand("git ls-files --modified --others --exclude-standard");
                std::istringstream changedStream(changedOutput);
                std::string changedFile;
                while (std::getline(changedStream, changedFile)) {
                    if (changedFile.empty()) continue;
                    if (std::find(processFiles.begin(), processFiles.end(), changedFile) == processFiles.end()) {
                        processFiles.push_back(changedFile);
                    }
                }
            }
        }

        std::string ghostDir = ghost::checkpoint::WorkingLog::getGhostDir(repoRoot);
        std::string sessionId = ghost::checkpoint::Session::generateId();
        std::string author = ghost::checkpoint::Session::getGitAuthor(repoRoot);
        time_t ts_end = std::time(nullptr);

        std::vector<ghost::checkpoint::SessionEntry> entries;
        int totalAdditions = 0;
        int totalDeletions = 0;

        std::string snapshotDir = ghostDir + "/snapshot";

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
            ts_start, ts_end, entries, totalAdditions, totalDeletions
        );

        // Save session to DB as well
        std::ostringstream sessJson;
        sessJson << "{\"session_id\":\"" << sessionId << "\",";
        sessJson << "\"agent\":\"" << agent << "\",";
        sessJson << "\"model\":\"" << model << "\",";
        sessJson << "\"author\":\"" << author << "\",";
        sessJson << "\"ts_start\":" << ts_start << ",";
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
        sess.ts_start = ts_start;
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
