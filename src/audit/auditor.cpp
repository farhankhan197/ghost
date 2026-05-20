#include "auditor.hpp"
#include "../git/notes.hpp"
#include "../git/repo.hpp"
#include "../git/diff.hpp"
#include "../git/blame.hpp"
#include "../note/reader.hpp"
#include "../note/verified_reader.hpp"
#include "../config/ghost_config.hpp"
#include <cstdio>
#include <memory>
#include <sstream>
#include <set>
#include <algorithm>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;


namespace ghost {
namespace audit {

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
    std::vector<std::string> result;
    std::string cmd = "git notes --ref=ghost list";
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) return result;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe.get())) {
        std::string line = buffer;
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        if (line.empty()) continue;
        size_t space = line.find(' ');
        if (space != std::string::npos) {
            result.push_back(line.substr(space + 1));
        }
    }
    return result;
}

static std::string getCommitAuthor(const std::string& sha) {
    std::string cmd = "git log -1 --format=\"%an <%ae>\" " + sha;
    return runCommand(cmd);
}

static AuditReport auditCommits(
    const std::string& repoRoot,
    const std::vector<std::string>& commitShas,
    int thresholdOverride,
    bool jsonOutput
) {
    AuditReport report;
    report.json = jsonOutput;
    (void)repoRoot;

    config::GhostConfig cfg = config::GhostConfigReader::load(repoRoot);

    if (commitShas.empty()) {
        report.summary = AuditSummary{};
        report.policy.passed = true;
        report.policy.message = "No commits with ghost notes found.";
        return report;
    }

    std::map<std::string, note::NoteReader::Result> ghostNotes;
    for (const auto& sha : commitShas) {
        std::string raw = git::Notes::show("refs/notes/ghost", sha);
        if (!raw.empty()) {
            ghostNotes[sha] = note::NoteReader::parse(raw);
        }
    }

    std::set<std::string> allFiles;
    for (const auto& sha : commitShas) {
        std::string cmd = "git diff-tree --no-commit-id -r --name-only " + sha + " -- .";
        std::string out = runCommand(cmd);
        std::istringstream stream(out);
        std::string file;
        while (std::getline(stream, file)) {
            if (!file.empty()) allFiles.insert(file);
        }
    }

    std::map<std::string, std::map<int, std::string>> blameCache;

    std::vector<CommitSummary> commitSummaries;
    for (const auto& sha : commitShas) {
        CommitSummary cs;
        cs.commit_sha = sha;
        cs.author = getCommitAuthor(sha);
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



        std::string fileList = runCommand("git diff-tree --no-commit-id -r --name-only " + sha + " -- .");
        std::istringstream fileStream(fileList);
        std::string filePath;
        while (std::getline(fileStream, filePath)) {
            if (filePath.empty()) continue;

            if (blameCache.find(filePath) == blameCache.end()) {
                blameCache[filePath] = git::Blame::getLineAuthorMap(filePath);
            }

            if (blameCache[filePath].empty()) continue;

            FileAttribution fa = BlameOverlay::overlay(filePath, blameCache[filePath], ghostNotes);
            cs.files.push_back(fa);
            cs.total_lines += fa.total_lines;
            cs.ai_lines += fa.ai_lines;
        }



        commitSummaries.push_back(cs);
    }

    report.summary = Aggregator::aggregate(commitSummaries);
    report.policy = Policy::enforce(report.summary, cfg, thresholdOverride);

    return report;
}

AuditReport Auditor::run(
    const std::string& repoRoot,
    const std::string& range,
    int thresholdOverride,
    bool jsonOutput
) {
    std::vector<std::string> commitShas = getCommitsInRange(range);
    return auditCommits(repoRoot, commitShas, thresholdOverride, jsonOutput);
}

AuditReport Auditor::runFromList(
    const std::string& repoRoot,
    const std::vector<std::string>& commitShas,
    int thresholdOverride,
    bool jsonOutput
) {
    return auditCommits(repoRoot, commitShas, thresholdOverride, jsonOutput);
}

std::vector<std::string> Auditor::getCommitsWithGhostNotes() {
    return ::ghost::audit::getCommitsWithGhostNotes("");
}

PolicyResult Auditor::checkPending(const std::string& repoRoot, int thresholdOverride) {
    config::GhostConfig cfg = config::GhostConfigReader::load(repoRoot);
    
    // 1. Sum up additions from all sessions in .git/ghost/sessions/
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
                
                // Simple JSON extraction of "additions": N
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
    
    // 2. Sum up total additions in staged changes
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
    
    // Also include unstaged changes if we want a full "working tree" check
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

CodebaseReport Auditor::runCodebaseBlame(
    const std::string& repoRoot,
    const std::string& target,
    int thresholdOverride,
    bool jsonOutput
) {
    CodebaseReport report;
    report.json = jsonOutput;

    config::GhostConfig cfg = config::GhostConfigReader::load(repoRoot);

    // Resolve target to a sha
    std::string sha = runCommand("git rev-parse " + target);
    if (sha.empty()) {
        report.policy.passed = true;
        report.policy.message = "Invalid commit reference: " + target;
        return report;
    }

    // Get files changed in this commit
    std::set<std::string> changedFiles;
    std::string diffOut = runCommand("git diff-tree --no-commit-id -r --name-only " + sha + " -- .");
    std::istringstream diffStream(diffOut);
    std::string df;
    while (std::getline(diffStream, df)) {
        if (!df.empty()) changedFiles.insert(df);
    }

    // Get ghost note for this commit to know AI sessions
    std::map<std::string, note::NoteReader::Result> commitGhostNote;
    std::string rawNote = git::Notes::show("refs/notes/ghost", sha);
    if (!rawNote.empty()) {
        commitGhostNote[sha] = note::NoteReader::parse(rawNote);
    }

    // Get all tracked files at that commit
    std::vector<std::string> files;
    std::string treeOut = runCommand("git ls-tree --name-only -r " + sha + " -- .");
    std::istringstream treeStream(treeOut);
    std::string filePath;
    while (std::getline(treeStream, filePath)) {
        if (!filePath.empty()) files.push_back(filePath);
    }

    // Blame all files, collect unique SHAs
    std::map<std::string, std::map<int, std::string>> blameCache;
    std::set<std::string> allShas;
    for (const auto& f : files) {
        blameCache[f] = git::Blame::getLineAuthorMap(f, sha);
        if (blameCache[f].empty()) continue;
        for (const auto& [_, commitSha] : blameCache[f]) {
            allShas.insert(commitSha);
        }
    }
    allShas.insert(sha);

    // Fetch ghost notes for all unique SHAs
    std::map<std::string, note::NoteReader::Result> ghostNotes;
    for (const auto& s : allShas) {
        std::string raw = git::Notes::show("refs/notes/ghost", s);
        if (!raw.empty()) {
            ghostNotes[s] = note::NoteReader::parse(raw);
        }
    }

    // Fetch author names for all unique SHAs
    std::map<std::string, std::string> authorCache;
    for (const auto& s : allShas) {
        authorCache[s] = runCommand("git log -1 --format=\"%an\" " + s);
    }

    // Build per-file summaries
    std::vector<FileBlameSummary> inCommitFiles;
    std::vector<FileBlameSummary> pastAiFiles;
    int grandTotal = 0;
    int grandAI = 0;
    int commitAiLines = 0;
    int commitTotalLines = 0;

    for (const auto& f : files) {
        if (blameCache[f].empty()) continue;

        auto attribution = BlameOverlay::overlay(f, blameCache[f], ghostNotes);
        if (attribution.total_lines == 0) continue;

        FileBlameSummary fbs;
        fbs.file_path = f;
        fbs.total_lines = attribution.total_lines;
        fbs.ai_lines = attribution.ai_lines;
        fbs.in_commit = false;

        // Count lines from this commit's sha
        int linesFromThisCommit = 0;
        int aiLinesFromThisCommit = 0;
        for (const auto& line : attribution.lines) {
            if (line.commit_sha == sha) {
                linesFromThisCommit++;
                if (line.is_ai) {
                    aiLinesFromThisCommit++;
                }
            }
        }

        // File is "in_commit" if it was changed in this commit AND has AI lines from this commit
        bool fileInCommit = (changedFiles.count(f) > 0) && (aiLinesFromThisCommit > 0);
        fbs.in_commit = fileInCommit;

        // Group AI lines by agent+model, count lines per author
        std::map<std::string, int> agentLines;
        std::map<std::string, int> authorLines;
        std::map<std::string, int> commitAgentLines;

        for (const auto& line : attribution.lines) {
            std::string author = authorCache.count(line.commit_sha) ? authorCache[line.commit_sha] : "unknown";
            authorLines[author]++;

            if (line.is_ai) {
                std::string key = line.agent + "/" + line.model;
                agentLines[key]++;
                if (line.commit_sha == sha) {
                    commitAgentLines[key]++;
                }
            }
        }

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

        // Commit entity = agent with most AI lines in this specific commit
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

        // Track commit-level stats for files changed in this commit
        if (changedFiles.count(f) > 0) {
            commitTotalLines += fbs.total_lines;
            commitAiLines += aiLinesFromThisCommit;
        }

        // Count all files toward codebase total
        grandTotal += fbs.total_lines;
        if (fbs.ai_lines > 0) {
            if (fileInCommit) {
                inCommitFiles.push_back(fbs);
            } else {
                pastAiFiles.push_back(fbs);
            }
            grandAI += fbs.ai_lines;
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
