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
#include "checkpoint_store.hpp"
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

static std::string findRepoRootForPath(const std::string& path) {
    if (path.empty()) return "";
    fs::path p(path);
    std::error_code ec;
    if (!p.is_absolute()) {
        p = fs::absolute(p, ec);
        if (ec) return "";
    }
    fs::path dir = fs::is_directory(p, ec) ? p : p.parent_path();
    while (!dir.empty()) {
        if (fs::exists(dir / ".git", ec)) {
            return dir.string();
        }
        fs::path parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return "";
}

static fs::path absoluteTargetPath(const std::string& targetFile, const std::string& repoRoot, const fs::path& baseDir) {
    fs::path p(targetFile);
    if (p.is_absolute()) return p;
    std::error_code ec;
    if (!baseDir.empty()) {
        fs::path fromBase = baseDir / p;
        std::string cwdOwner = findRepoRootForPath(fromBase.string());
        if (!cwdOwner.empty()) return fromBase;
    }
    if (!repoRoot.empty()) return fs::path(repoRoot) / p;
    return fs::absolute(p, ec);
}

static std::string normalizeTargetFile(const std::string& targetFile, const std::string& repoRoot, const fs::path& baseDir) {
    if (targetFile.empty()) return "";
    fs::path p = absoluteTargetPath(targetFile, repoRoot, baseDir);
    std::error_code ec;
    fs::path rel = fs::relative(p, repoRoot, ec);
    std::string normalized = ec ? targetFile : rel.string();
    for (char& c : normalized) {
        if (c == '\\') c = '/';
    }
    return normalized;
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

static std::string silenceStderr(const std::string& cmd) {
#ifdef _WIN32
    return cmd + " 2>nul";
#else
    return cmd + " 2>/dev/null";
#endif
}

static std::string extractCodexHookFile(const std::string& hookJson) {
    for (const auto& key : {"file_path", "filePath", "path", "file"}) {
        std::string value = extractJsonString(hookJson, key);
        if (!value.empty()) return value;
    }
    return "";
}

static std::string extractHookCwd(const std::string& hookJson) {
    for (const auto& key : {"cwd", "workingDirectory", "working_directory"}) {
        std::string value = extractJsonString(hookJson, key);
        if (!value.empty()) return value;
    }
    return "";
}

static bool sessionJsonHasEntry(const std::string& json, const std::string& filePath, const std::string& ranges) {
    return json.find("\"file_path\":\"" + filePath + "\"") != std::string::npos &&
           json.find("\"ranges\":\"" + ranges + "\"") != std::string::npos;
}

static bool isDuplicateRecentSession(
    ghost::persist::Database* db,
    const std::string& agent,
    const std::string& model,
    const std::vector<ghost::checkpoint::SessionEntry>& entries,
    int additions,
    int deletions,
    time_t tsEnd
) {
    if (!db || entries.empty()) return false;
    auto sessions = db->loadSessions(true);
    for (const auto& s : sessions) {
        if (s.agent != agent || s.model != model) continue;
        if (s.additions != additions || s.deletions != deletions) continue;
        if (std::llabs(static_cast<long long>(tsEnd) - static_cast<long long>(s.ts_end)) > 5) continue;

        bool allEntriesMatch = true;
        for (const auto& entry : entries) {
            if (!sessionJsonHasEntry(s.json_data, entry.file_path, entry.ranges)) {
                allEntriesMatch = false;
                break;
            }
        }
        if (allEntriesMatch) return true;
    }
    return false;
}

static void ensureGhostDir(const std::string& repoRoot) {
    std::error_code ec;
    fs::create_directories(fs::path(repoRoot) / ".git" / "ghost", ec);
}

int main(int argc, char* argv[]) {
    std::error_code pathEc;
    fs::path invocationCwd = fs::current_path(pathEc);
    if (argc < 2 || hasFlag(argc, argv, "--help") || hasFlag(argc, argv, "-h")) {
        std::cout << "Usage: ghost-checkpoint <command> [options]\n";
        std::cout << "Commands: pre, post, show, reset\n";
        std::cout << "Options:\n";
        std::cout << "  --agent <name>     Agent name (required)\n";
        std::cout << "  --model <model>    Model name (optional)\n";
        std::cout << "  --file <path>      Target file for per-edit checkpoint (optional)\n";
        std::cout << "  --hook-json        Read agent hook JSON from stdin (optional)\n";
        std::cout << "  --codex-hook       Alias for --hook-json\n";
        return argc < 2 ? 1 : 0;
    }

    std::string command = argv[1];
    std::string targetFile = getArg(argc, argv, "--file");
    bool hookJsonFlag = hasFlag(argc, argv, "--hook-json") || hasFlag(argc, argv, "--codex-hook");
    std::string hookJson = hookJsonFlag ? readStdinAll() : "";
    fs::path hookCwd;
    if (!hookJson.empty()) {
        std::string cwd = extractHookCwd(hookJson);
        if (!cwd.empty()) {
            hookCwd = fs::path(cwd);
            std::error_code hookCwdEc;
            if (fs::exists(hookCwd, hookCwdEc)) {
                fs::current_path(hookCwd, hookCwdEc);
            }
        }
    }
    if (targetFile.empty() && !hookJson.empty()) {
        targetFile = extractCodexHookFile(hookJson);
    }
    std::string repoRoot = ghost::git::Repo::getRoot();

    if (repoRoot.empty()) {
        if (hookJsonFlag) {
            return 0;
        }
        std::cerr << "Not in a git repository\n";
        return 1;
    }

    // Resolve repo root from the edited file when available so checkpoint data
    // lands in the file's repo, not a parent workspace or sibling repo.
    fs::path targetBaseDir = hookCwd.empty() ? invocationCwd : hookCwd;
    if (!targetFile.empty()) {
        fs::path p = absoluteTargetPath(targetFile, repoRoot, targetBaseDir);
        std::string fileRepo = findRepoRootForPath(p.string());
        if (fileRepo.empty() && !repoRoot.empty() && !p.is_absolute()) {
            fileRepo = findRepoRootForPath((fs::path(repoRoot) / p).string());
        }
        if (!fileRepo.empty()) {
            repoRoot = fileRepo;
        }
    }

    std::error_code cwdEc;
    fs::current_path(repoRoot, cwdEc);

    ensureGhostDir(repoRoot);
    auto* db = ghost::persist::getRepoDb(repoRoot);
    if (!db) {
        std::cerr << "Failed to open Ghost database\n";
        return 1;
    }

    if (command == "pre") {
        std::string agent = getArg(argc, argv, "--agent");
        std::string targetFile = getArg(argc, argv, "--file");
        if (targetFile.empty() && !hookJson.empty()) {
            targetFile = extractCodexHookFile(hookJson);
        }
        if (agent.empty()) {
            std::cerr << "Usage: ghost-checkpoint pre --agent <name> [--file <path>]\n";
            return 1;
        }

        targetFile = normalizeTargetFile(targetFile, repoRoot, targetBaseDir);

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
        if (targetFile.empty() && !hookJson.empty()) {
            targetFile = extractCodexHookFile(hookJson);
        }
        if (agent.empty()) {
            std::cerr << "Usage: ghost-checkpoint post --agent <name> --model <model> [--file <path>]\n";
            return 1;
        }

        targetFile = normalizeTargetFile(targetFile, repoRoot, targetBaseDir);
        if ((model.empty() || model == "unknown") && !hookJson.empty()) {
            std::string hookModel;
            for (const auto& key : {"model", "model_id", "modelId", "modelID"}) {
                hookModel = extractJsonString(hookJson, key);
                if (!hookModel.empty()) break;
            }
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

        auto checkpoints = db->loadCheckpoints(true);
        time_t ts_start = std::time(nullptr);
        std::vector<std::string> processFiles;

        if (!checkpoints.empty()) {
            bool hasWildcardCheckpoint = false;
            bool matchedCheckpoint = false;
            for (const auto& cp : checkpoints) {
                bool matches = targetFile.empty() || cp.target_file == targetFile || cp.target_file == "*";
                if (!matches) continue;
                matchedCheckpoint = true;
                ts_start = std::min(ts_start, cp.ts_start);
                if (cp.target_file == "*") {
                    hasWildcardCheckpoint = true;
                } else if (std::find(processFiles.begin(), processFiles.end(), cp.target_file) == processFiles.end()) {
                    processFiles.push_back(cp.target_file);
                }
            }
            if (!matchedCheckpoint) {
                checkpoints.clear();
            }
            if (!targetFile.empty()) {
                processFiles = {targetFile};
            } else if (hasWildcardCheckpoint) {
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

        if (processFiles.empty()) {
            // Standalone mode: no checkpoint metadata, use git diff HEAD to compute changes.
            std::string changedOutput = runCommand(silenceStderr("git diff --name-only HEAD --diff-filter=ACMR -- \"*\""));
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
        }

        std::string ghostDir = ghost::checkpoint::CheckpointStore::getGhostDir(repoRoot);
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

        if (isDuplicateRecentSession(db, agent, model, entries, totalAdditions, totalDeletions, ts_end)) {
            auto checkpoints = db->loadCheckpoints(true);
            for (const auto& cp : checkpoints) {
                if (targetFile.empty() || cp.target_file == targetFile || cp.target_file == "*") {
                    db->markCheckpointProcessed(cp.id);
                }
            }
            ghost::checkpoint::CheckpointStore::clearSnapshot(repoRoot);
            std::cout << "Duplicate session ignored\n";
            std::cout << "  Agent: " << agent << "\n";
            std::cout << "  Model: " << model << "\n";
            std::cout << "  Files changed: " << entries.size() << "\n";
            return 0;
        }

        // Save session to DB.
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
        checkpoints = db->loadCheckpoints(true);
        for (const auto& cp : checkpoints) {
            if (targetFile.empty() || cp.target_file == targetFile || cp.target_file == "*") {
                db->markCheckpointProcessed(cp.id);
            }
        }

        ghost::checkpoint::CheckpointStore::clearSnapshot(repoRoot);

        std::cout << "Session recorded: " << sessionId << "\n";
        std::cout << "  Agent: " << agent << "\n";
        std::cout << "  Model: " << model << "\n";
        std::cout << "  Files changed: " << entries.size() << "\n";
        std::cout << "  Additions: " << totalAdditions << "\n";
        std::cout << "  Deletions: " << totalDeletions << "\n";

    } else if (command == "show") {
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

        if (checkpoints.empty() && sessions.empty()) {
            std::cout << "No active sessions or checkpoints\n";
        }

    } else if (command == "reset") {
        ghost::checkpoint::CheckpointStore::clearSnapshot(repoRoot);
        db->clearCheckpoints();
        std::cout << "Pre-state cleared\n";

    } else {
        std::cerr << "Unknown command: " << command << "\n";
        return 1;
    }

    return 0;
}
