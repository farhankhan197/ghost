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

}
}
