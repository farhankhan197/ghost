#include "auditor.hpp"
#include "../git/notes.hpp"
#include "../git/repo.hpp"
#include "../git/diff.hpp"
#include "../git/blame.hpp"
#include "../note/reader.hpp"
#include "../note/gitai_reader.hpp"
#include "../note/verified_reader.hpp"
#include "../config/ghost_config.hpp"
#include "thread_pool.hpp"
#include <cstdio>
#include <memory>
#include <sstream>
#include <set>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <iostream>
#include <future>

namespace fs = std::filesystem;

namespace ghost {
namespace audit {

// ── Benchmark timing ──────────────────────────────────────────────────

static bool g_benchmark = false;

struct BenchmarkTimer {
    std::string name;
    std::chrono::steady_clock::time_point start;
    explicit BenchmarkTimer(const std::string& n) : name(n) {
        if (!g_benchmark) return;
        start = std::chrono::steady_clock::now();
    }
    ~BenchmarkTimer() {
        if (!g_benchmark) return;
        auto elapsed = std::chrono::steady_clock::now() - start;
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        std::cerr << "[benchmark] " << name << ": " << ms << "ms\n";
    }
};

static void initBenchmark() {
    if (std::getenv("GHOST_BENCHMARK") != nullptr) {
        g_benchmark = true;
    }
}

// ── Helpers ─────────────────────────────────────────────────────────

static std::string runCommand(const std::string& cmd) {
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) return "";
    std::string result;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe.get())) result += buffer;
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) result.pop_back();
    return result;
}

static std::vector<std::string> getCommitsInRange(const std::string& range) {
    std::vector<std::string> result;
    std::string cmd = "git rev-list " + range + " -- .";
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) return result;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe.get())) {
        std::string sha = buffer;
        while (!sha.empty() && (sha.back() == '\n' || sha.back() == '\r')) sha.pop_back();
        if (!sha.empty()) result.push_back(sha);
    }
    return result;
}

static std::vector<std::string> getCommitsWithGhostNotes(const std::string& repoRoot) {
    (void)repoRoot;
    std::set<std::string> commits;
    for (const auto& ref : {"ghost", "ai"}) {
        std::string cmd = std::string("git notes --ref=") + ref + " list";
        std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
        if (!pipe) continue;
        char buffer[256];
        while (fgets(buffer, sizeof(buffer), pipe.get())) {
            std::string line = buffer;
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
            if (line.empty()) continue;
            size_t space = line.find(' ');
            if (space != std::string::npos) {
                commits.insert(line.substr(space + 1));
            }
        }
    }
    return std::vector<std::string>(commits.begin(), commits.end());
}

static std::string getCommitAuthor(const std::string& sha) {
    std::string cmd = "git log -1 --format=\"%an <%ae>\" " + sha;
    return runCommand(cmd);
}

static std::map<std::string, note::NoteReader::Result> loadAttributionNotes(
    const std::vector<std::string>& commitShas,
    bool gitaiFallback
) {
    std::map<std::string, note::NoteReader::Result> notes;

    auto ghostBatch = git::Notes::showBatch("refs/notes/ghost", commitShas);
    for (const auto& [sha, raw] : ghostBatch) {
        if (!raw.empty()) {
            notes[sha] = note::NoteReader::parse(raw);
        }
    }

    if (!gitaiFallback) return notes;

    auto gitAiBatch = git::Notes::showBatch("refs/notes/ai", commitShas);
    for (const auto& [sha, raw] : gitAiBatch) {
        if (raw.empty() || notes.count(sha) > 0) continue;
        notes[sha] = note::GitAiReader::parse(raw);
    }

    return notes;
}

// Batch author lookup: single popen for up to N SHAs
static std::map<std::string, std::string> getCommitAuthorsBatch(const std::set<std::string>& shas, size_t chunkSize = 50) {
    std::map<std::string, std::string> result;
    if (shas.empty()) return result;

    std::vector<std::string> shaList(shas.begin(), shas.end());
    for (size_t i = 0; i < shaList.size(); i += chunkSize) {
        size_t end = std::min(i + chunkSize, shaList.size());
        std::string cmd = "git log --no-walk --format=\"%H %an\" ";
        for (size_t j = i; j < end; ++j) {
            cmd += shaList[j] + " ";
        }
        std::string out = runCommand(cmd);
        std::istringstream stream(out);
        std::string line;
        while (std::getline(stream, line)) {
            size_t space = line.find(' ');
            if (space != std::string::npos && space > 0) {
                std::string sha = line.substr(0, space);
                std::string author = line.substr(space + 1);
                result[sha] = author;
            }
        }
    }
    return result;
}

// Parallel blame execution using thread pool
static std::map<std::string, git::BlameResult> blameFilesParallel(
    const std::vector<std::string>& files,
    const std::string& commitSha,
    size_t numThreads
) {
    std::map<std::string, git::BlameResult> result;
    if (files.empty()) return result;

    ghost::util::ThreadPool pool(numThreads);
    std::vector<std::future<std::pair<std::string, git::BlameResult>>> futures;
    futures.reserve(files.size());

    for (const auto& f : files) {
        futures.push_back(pool.enqueue([&f, &commitSha]() {
            if (commitSha.empty()) {
                return std::make_pair(f, git::Blame::getLineAuthorMap(f));
            } else {
                return std::make_pair(f, git::Blame::getLineAuthorMap(f, commitSha));
            }
        }));
    }

    for (auto& fut : futures) {
        auto [path, blame] = fut.get();
        result[path] = blame;
    }

    return result;
}

// Check if file should be skipped based on ignore patterns
static bool shouldIgnoreFile(const std::string& filePath, const std::vector<std::string>& ignorePatterns) {
    for (const auto& pattern : ignorePatterns) {
        // Simple suffix or directory matching
        if (pattern.back() == '/') {
            // Directory pattern: "node_modules/"
            std::string dirPrefix = pattern.substr(0, pattern.size() - 1);
            if (filePath.find(dirPrefix + "/") != std::string::npos ||
                filePath.find(dirPrefix + "\\") != std::string::npos ||
                filePath.substr(0, dirPrefix.size()) == dirPrefix) {
                return true;
            }
        } else if (pattern.front() == '*') {
            // Extension pattern: "*.min.js"
            std::string suffix = pattern.substr(1);
            if (filePath.size() >= suffix.size() &&
                filePath.compare(filePath.size() - suffix.size(), suffix.size(), suffix) == 0) {
                return true;
            }
        } else {
            // Exact or substring match
            if (filePath.find(pattern) != std::string::npos) {
                return true;
            }
        }
    }
    return false;
}

// ── Per-Commit Audit ──────────────────────────────────────────────────

static AuditReport auditCommits(
    const std::string& repoRoot,
    const std::vector<std::string>& commitShas,
    int thresholdOverride,
    bool jsonOutput,
    const std::string& configRef
) {
    initBenchmark();
    BenchmarkTimer totalTimer("auditCommits total");

    AuditReport report;
    report.json = jsonOutput;

    config::GhostConfig cfg = configRef.empty()
        ? config::GhostConfigReader::load(repoRoot)
        : config::GhostConfigReader::loadFromRef(repoRoot, configRef);

    if (commitShas.empty()) {
        report.summary = AuditSummary{};
        report.policy.passed = true;
        report.policy.message = "No commits with ghost notes found.";
        return report;
    }

    // Fetch attribution notes for all commits — batched into single subprocess
    std::map<std::string, note::NoteReader::Result> ghostNotes;
    {
        BenchmarkTimer t("fetch attribution notes");
        ghostNotes = loadAttributionNotes(commitShas, cfg.gitai_fallback);
    }

    // A1: Cache diff-tree results per commit — single pass
    std::set<std::string> allFiles;
    std::map<std::string, std::vector<std::string>> commitFiles;
    {
        BenchmarkTimer t("diff-tree (batched)");
        for (const auto& sha : commitShas) {
            std::string cmd = "git diff-tree --no-commit-id -r --name-only " + sha + " -- .";
            std::string out = runCommand(cmd);
            std::istringstream stream(out);
            std::string file;
            while (std::getline(stream, file)) {
                if (!file.empty()) {
                    allFiles.insert(file);
                    commitFiles[sha].push_back(file);
                }
            }
        }
    }

    // Blame cache (flat vector, not map) — parallelized
    std::map<std::string, git::BlameResult> blameCache;
    {
        BenchmarkTimer t("blame all files");
        std::vector<std::string> fileVec(allFiles.begin(), allFiles.end());
        size_t numThreads = std::max(1u, std::thread::hardware_concurrency());
        blameCache = blameFilesParallel(fileVec, "", numThreads);
    }

    // A2: Overlay cache per file — computed once, reused across commits
    std::map<std::string, FileAttribution> overlayCache;

    // Batch author lookup for all commits upfront
    std::map<std::string, std::string> authorCache;
    {
        BenchmarkTimer t("fetch all authors");
        std::set<std::string> shaSet(commitShas.begin(), commitShas.end());
        authorCache = getCommitAuthorsBatch(shaSet);
    }

    std::vector<CommitSummary> commitSummaries;
    {
        BenchmarkTimer t("overlay + aggregate per commit");
        for (const auto& sha : commitShas) {
            CommitSummary cs;
            cs.commit_sha = sha;
            auto authorIt = authorCache.find(sha);
            cs.author = (authorIt != authorCache.end()) ? authorIt->second : "unknown";
            cs.total_lines = 0;
            cs.ai_lines = 0;
            cs.has_ghost_note = ghostNotes.count(sha) > 0;
            cs.has_verified_note = git::Notes::exists("refs/notes/ghost-verified", sha);

            if (ghostNotes.count(sha) > 0 && !ghostNotes[sha].entries.empty()) {
                std::string sid = ghostNotes[sha].entries[0].session_id;
                if (ghostNotes[sha].sessions.count(sid) > 0) {
                    const auto& sess = ghostNotes[sha].sessions.at(sid);
                    cs.primary_agent = sess.agent;
                    cs.primary_model = sess.model;
                } else {
                    cs.primary_agent = "ai";
                    cs.primary_model = "unknown";
                }
            } else {
                cs.primary_agent = "human";
                cs.primary_model = "";
            }

            // Use cached file list instead of running diff-tree again
            for (const auto& filePath : commitFiles[sha]) {
                if (shouldIgnoreFile(filePath, cfg.ignore)) continue;
                if (blameCache.find(filePath) == blameCache.end()) continue;
                if (blameCache[filePath].empty()) continue;

                // A2: Reuse overlay if already computed for this file
                auto overlayIt = overlayCache.find(filePath);
                if (overlayIt == overlayCache.end()) {
                    overlayIt = overlayCache.insert({filePath,
                        BlameOverlay::overlay(filePath, blameCache[filePath], ghostNotes)}).first;
                }

                cs.files.push_back(overlayIt->second);
                cs.total_lines += overlayIt->second.total_lines;
                cs.ai_lines += overlayIt->second.ai_lines;
            }

            commitSummaries.push_back(cs);
        }
    }

    report.summary = Aggregator::aggregate(commitSummaries);
    report.policy = Policy::enforce(report.summary, cfg, thresholdOverride);

    return report;
}

// ── Public API ────────────────────────────────────────────────────────

AuditReport Auditor::run(
    const std::string& repoRoot,
    const std::string& range,
    int thresholdOverride,
    bool jsonOutput,
    const std::string& configRef
) {
    std::vector<std::string> commitShas = getCommitsInRange(range);
    return auditCommits(repoRoot, commitShas, thresholdOverride, jsonOutput, configRef);
}

AuditReport Auditor::runFromList(
    const std::string& repoRoot,
    const std::vector<std::string>& commitShas,
    int thresholdOverride,
    bool jsonOutput,
    const std::string& configRef
) {
    return auditCommits(repoRoot, commitShas, thresholdOverride, jsonOutput, configRef);
}

std::vector<std::string> Auditor::getCommitsWithGhostNotes() {
    return ::ghost::audit::getCommitsWithGhostNotes("");
}

// ── Check Pending (staged) ────────────────────────────────────────────

PolicyResult Auditor::checkPending(const std::string& repoRoot, int thresholdOverride, const std::string& configRef) {
    config::GhostConfig cfg = configRef.empty()
        ? config::GhostConfigReader::load(repoRoot)
        : config::GhostConfigReader::loadFromRef(repoRoot, configRef);
    
    int aiAdditions = 0;
    std::string ghostDir = repoRoot + "/.git/ghost";
    std::string sessionDir = ghostDir + "/sessions";
    
    std::error_code ec;
    if (fs::exists(sessionDir, ec)) {
        for (const auto& entry : fs::directory_iterator(sessionDir, ec)) {
            if (entry.is_regular_file()) {
                std::ifstream f(entry.path());
                std::stringstream ss;
                ss << f.rdbuf();
                std::string content = ss.str();
                
                size_t pos = content.find("\"additions\":");
                if (pos != std::string::npos) {
                    size_t end = content.find_first_of(",}", pos);
                    if (end != std::string::npos) {
                        try {
                            aiAdditions += std::stoi(content.substr(pos + 12, end - (pos + 12)));
                        } catch (...) {}
                    }
                }
            }
        }
    }
    
    int totalAdditions = 0;
    std::string diffOut = runCommand("git diff --cached --numstat");
    std::istringstream diffStream(diffOut);
    std::string line;
    while (std::getline(diffStream, line)) {
        if (line.empty()) continue;
        std::istringstream lineStream(line);
        std::string adds;
        if (lineStream >> adds) {
            if (adds != "-") {
                try { totalAdditions += std::stoi(adds); } catch (...) {}
            }
        }
    }
    
    std::string diffOut2 = runCommand("git diff --numstat");
    std::istringstream diffStream2(diffOut2);
    while (std::getline(diffStream2, line)) {
        if (line.empty()) continue;
        std::istringstream lineStream(line);
        std::string adds;
        if (lineStream >> adds) {
            if (adds != "-") {
                try { totalAdditions += std::stoi(adds); } catch (...) {}
            }
        }
    }

    AuditSummary summary;
    summary.total_lines = totalAdditions;
    summary.ai_lines = aiAdditions;
    
    return Policy::enforce(summary, cfg, thresholdOverride);
}

// ── Codebase Blame ─────────────────────────────────────────────────────

CodebaseReport Auditor::runCodebaseBlame(
    const std::string& repoRoot,
    const std::string& target,
    int thresholdOverride,
    bool jsonOutput,
    const std::string& configRef
) {
    initBenchmark();
    BenchmarkTimer totalTimer("runCodebaseBlame total");

    CodebaseReport report;
    report.json = jsonOutput;

    config::GhostConfig cfg = configRef.empty()
        ? config::GhostConfigReader::load(repoRoot)
        : config::GhostConfigReader::loadFromRef(repoRoot, configRef);

    // Resolve target to a sha
    std::string sha;
    {
        BenchmarkTimer t("rev-parse");
        sha = runCommand("git rev-parse " + target);
    }
    if (sha.empty()) {
        report.policy.passed = true;
        report.policy.message = "Invalid commit reference: " + target;
        return report;
    }

    // Get files changed in this commit
    std::set<std::string> changedFiles;
    {
        BenchmarkTimer t("diff-tree changed files");
        std::string diffOut = runCommand("git diff-tree --no-commit-id -r --name-only " + sha + " -- .");
        std::istringstream diffStream(diffOut);
        std::string df;
        while (std::getline(diffStream, df)) {
            if (!df.empty()) changedFiles.insert(df);
        }
    }

    // Get all tracked files at that commit, filtered by ignore patterns
    std::vector<std::string> files;
    {
        BenchmarkTimer t("ls-tree + filter");
        std::string treeOut = runCommand("git ls-tree --name-only -r " + sha + " -- .");
        std::istringstream treeStream(treeOut);
        std::string filePath;
        while (std::getline(treeStream, filePath)) {
            if (!filePath.empty() && !shouldIgnoreFile(filePath, cfg.ignore)) {
                files.push_back(filePath);
            }
        }
    }

    // Blame all files in parallel, collect unique SHAs
    std::map<std::string, git::BlameResult> blameCache;
    std::set<std::string> allShas;
    {
        BenchmarkTimer t("blame all files");
        size_t numThreads = std::max(1u, std::thread::hardware_concurrency());
        blameCache = blameFilesParallel(files, sha, numThreads);
        for (const auto& [f, blame] : blameCache) {
            if (blame.empty()) continue;
            for (const auto& commitSha : blame.lines) {
                allShas.insert(commitSha);
            }
        }
    }
    allShas.insert(sha);

    // Fetch attribution notes for all unique SHAs — batched into single subprocess
    std::map<std::string, note::NoteReader::Result> ghostNotes;
    {
        BenchmarkTimer t("fetch all attribution notes");
        std::vector<std::string> shaVec(allShas.begin(), allShas.end());
        ghostNotes = loadAttributionNotes(shaVec, cfg.gitai_fallback);
    }

    // Extract target commit's ghost note and build ghostNoteFiles from batch result
    std::map<std::string, note::NoteReader::Result> commitGhostNote;
    std::set<std::string> ghostNoteFiles;
    if (ghostNotes.count(sha) > 0) {
        commitGhostNote[sha] = ghostNotes[sha];
        for (const auto& entry : ghostNotes[sha].entries) {
            ghostNoteFiles.insert(entry.file_path);
        }
    }

    // Fetch author names for all unique SHAs — batched into single popen
    std::map<std::string, std::string> authorCache;
    {
        BenchmarkTimer t("fetch all authors");
        authorCache = getCommitAuthorsBatch(allShas);
    }

    // Build per-file summaries
    std::vector<FileBlameSummary> inCommitFiles;
    std::vector<FileBlameSummary> pastAiFiles;
    int grandTotal = 0;
    int grandAI = 0;
    int commitAiLines = 0;
    int commitTotalLines = 0;

    {
        BenchmarkTimer t("overlay + aggregate all files");
        for (const auto& f : files) {
            if (blameCache[f].empty()) continue;

            auto attribution = BlameOverlay::overlay(f, blameCache[f], ghostNotes);
            if (attribution.total_lines == 0) continue;

            FileBlameSummary fbs;
            fbs.file_path = f;
            fbs.total_lines = attribution.total_lines;
            fbs.ai_lines = attribution.ai_lines;
            fbs.in_commit = false;

            // A6: Single-pass aggregation
            std::map<std::string, int> agentLines;
            std::map<std::string, int> authorLines;
            std::map<std::string, int> commitAgentLines;

            int linesFromThisCommit = 0;
            int aiLinesFromThisCommit = 0;

            for (const auto& line : attribution.lines) {
                // Count lines from this commit's sha
                if (line.commit_sha == sha) {
                    linesFromThisCommit++;
                    if (line.is_ai) {
                        aiLinesFromThisCommit++;
                    }
                }

                // Author grouping
                std::string author = authorCache.count(line.commit_sha) ? authorCache[line.commit_sha] : "unknown";
                authorLines[author]++;

                // AI agent grouping
                if (line.is_ai) {
                    std::string key = line.agent + "/" + line.model;
                    agentLines[key]++;
                    if (line.commit_sha == sha) {
                        commitAgentLines[key]++;
                    }
                }
            }

            // File is "in_commit" if changed in this commit AND has ghost note
            bool fileInCommit = (changedFiles.count(f) > 0) && (ghostNoteFiles.count(f) > 0);
            fbs.in_commit = fileInCommit;

            // Primary author = whoever wrote the most lines
            std::string bestAuthor;
            int bestAuthorCount = 0;
            for (const auto& [author, count] : authorLines) {
                if (count > bestAuthorCount) {
                    bestAuthorCount = count;
                    bestAuthor = author;
                }
            }
            fbs.primary_author = bestAuthor;

            // Primary entity = agent with most AI lines, or "human"
            std::string bestEntity;
            int bestEntityCount = 0;
            for (const auto& [key, count] : agentLines) {
                if (count > bestEntityCount) {
                    bestEntityCount = count;
                    bestEntity = key;
                }
            }
            if (bestEntity.empty()) {
                fbs.primary_entity = "human";
            } else {
                fbs.primary_entity = bestEntity;
            }

            // Commit entity from ghost note session info
            if (fileInCommit && commitGhostNote.count(sha) > 0) {
                auto fileEntries = commitGhostNote[sha].entries_by_file.find(f);
                if (fileEntries != commitGhostNote[sha].entries_by_file.end() && !fileEntries->second.empty()) {
                    std::string sid = fileEntries->second[0].session_id;
                    if (commitGhostNote[sha].sessions.count(sid) > 0) {
                        const auto& sess = commitGhostNote[sha].sessions.at(sid);
                        fbs.commit_entity = sess.agent + "/" + sess.model;
                    }
                }
            }
            if (fbs.commit_entity.empty()) {
                // Fallback to blame-based commit entity
                std::string bestCommitEntity;
                int bestCommitEntityCount = 0;
                for (const auto& [key, count] : commitAgentLines) {
                    if (count > bestCommitEntityCount) {
                        bestCommitEntityCount = count;
                        bestCommitEntity = key;
                    }
                }
                if (bestCommitEntity.empty()) {
                    fbs.commit_entity = "human";
                } else {
                    fbs.commit_entity = bestCommitEntity;
                }
            }

            // Build entity list sorted by lines desc
            std::vector<std::pair<std::string, int>> sortedAgents(agentLines.begin(), agentLines.end());
            std::sort(sortedAgents.begin(), sortedAgents.end(), [](const auto& a, const auto& b) {
                return a.second > b.second;
            });
            for (const auto& [key, count] : sortedAgents) {
                FileEntity fe;
                size_t slash = key.find('/');
                fe.agent = key.substr(0, slash);
                fe.model = slash != std::string::npos ? key.substr(slash + 1) : "unknown";
                fe.lines = count;
                fbs.entities.push_back(fe);
            }

            // Track commit-level stats
            if (changedFiles.count(f) > 0) {
                commitTotalLines += fbs.total_lines;
                if (ghostNoteFiles.count(f) > 0 && commitGhostNote.count(sha) > 0) {
                    auto fileEntries = commitGhostNote[sha].entries_by_file.find(f);
                    if (fileEntries != commitGhostNote[sha].entries_by_file.end() && !fileEntries->second.empty()) {
                        std::string sid = fileEntries->second[0].session_id;
                        if (commitGhostNote[sha].sessions.count(sid) > 0) {
                            commitAiLines += commitGhostNote[sha].sessions.at(sid).additions;
                        }
                    }
                } else {
                    commitAiLines += aiLinesFromThisCommit;
                }
            }

            // Count all files toward codebase total
            grandTotal += fbs.total_lines;
            
            // For in_commit files, use ghost note additions for ai_lines
            if (fileInCommit && commitGhostNote.count(sha) > 0) {
                auto fileEntries = commitGhostNote[sha].entries_by_file.find(f);
                if (fileEntries != commitGhostNote[sha].entries_by_file.end() && !fileEntries->second.empty()) {
                    std::string sid = fileEntries->second[0].session_id;
                    if (commitGhostNote[sha].sessions.count(sid) > 0) {
                        fbs.ai_lines = commitGhostNote[sha].sessions.at(sid).additions;
                    }
                }
            }
            
            if (fbs.ai_lines > 0) {
                if (fileInCommit) {
                    inCommitFiles.push_back(fbs);
                } else {
                    pastAiFiles.push_back(fbs);
                }
                grandAI += fbs.ai_lines;
            }
        }
    }

    // Sort each group by AI% descending
    auto sortByAiPct = [](const FileBlameSummary& a, const FileBlameSummary& b) {
        double pctA = a.total_lines > 0 ? (double)a.ai_lines / a.total_lines : 0;
        double pctB = b.total_lines > 0 ? (double)b.ai_lines / b.total_lines : 0;
        return pctA > pctB;
    };
    std::sort(inCommitFiles.begin(), inCommitFiles.end(), sortByAiPct);
    std::sort(pastAiFiles.begin(), pastAiFiles.end(), sortByAiPct);

    // Combine: in-commit files first, then past AI files
    std::vector<FileBlameSummary> allFiles;
    allFiles.insert(allFiles.end(), inCommitFiles.begin(), inCommitFiles.end());
    allFiles.insert(allFiles.end(), pastAiFiles.begin(), pastAiFiles.end());

    report.summary.target_sha = sha;
    report.summary.files = allFiles;
    report.summary.total_lines = grandTotal;
    report.summary.ai_lines = grandAI;
    report.summary.commit_ai_lines = commitAiLines;
    report.summary.commit_total_lines = commitTotalLines;
    report.policy = Policy::enforce(
        audit::AuditSummary{std::vector<CommitSummary>(), grandTotal, grandAI},
        cfg,
        thresholdOverride
    );

    return report;
}


}
}
