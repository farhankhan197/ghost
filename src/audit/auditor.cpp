#include "auditor.hpp"
#include "attribution_notes.hpp"
#include "attribution_resolver.hpp"
#include "../git/notes.hpp"
#include "../git/blame.hpp"
#include "../git/diff.hpp"
#include "../git/engine.hpp"
#include "../git/ref.hpp"
#include "../note/reader.hpp"
#include "../config/ghost_config.hpp"
#include "../config/ignore_matcher.hpp"
#include "thread_pool.hpp"
#include <sstream>
#include <set>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <future>

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

static std::vector<std::string> getCommitsInRange(const std::string& repoRoot, const std::string& range) {
    std::vector<std::string> result;
    if (!git::Ref::isSafeRange(range)) return result;
    return git::Engine::revList(repoRoot, range);
}

static std::vector<std::string> getCommitsWithGhostNotes(const std::string& repoRoot) {
    std::set<std::string> commits;
    for (const auto& ref : {"ghost", "ai"}) {
        auto notes = git::Engine::noteList(repoRoot, ref);
        for (const auto& [sha, _] : notes) {
            commits.insert(sha);
        }
    }
    return std::vector<std::string>(commits.begin(), commits.end());
}

static std::map<std::string, note::NoteReader::Result> loadAttributionNotes(
    const std::string& repoRoot,
    const std::vector<std::string>& commitShas,
    bool gitaiFallback
) {
    return AttributionNotes::load(repoRoot, commitShas, gitaiFallback);
}

static std::map<std::string, std::string> getCommitAuthorsBatch(
    const std::string& repoRoot,
    const std::set<std::string>& shas,
    size_t chunkSize = 50
) {
    std::map<std::string, std::string> result;
    (void)chunkSize;
    if (shas.empty()) return result;
    std::vector<std::string> shaList(shas.begin(), shas.end());
    return git::Engine::commitAuthors(repoRoot, shaList);
}

static std::string headFromRange(const std::string& range) {
    size_t triple = range.find("...");
    if (triple != std::string::npos) return range.substr(triple + 3);
    size_t dbl = range.find("..");
    if (dbl != std::string::npos) return range.substr(dbl + 2);
    return "HEAD";
}

// Parallel blame execution using thread pool
static std::map<std::string, git::BlameResult> blameFilesParallel(
    const std::string& repoRoot,
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
        futures.push_back(pool.enqueue([repoRoot, file = f, &commitSha]() {
            if (commitSha.empty()) {
                return std::make_pair(file, git::Blame::getLineAuthorMap(repoRoot, file, ""));
            } else {
                return std::make_pair(file, git::Blame::getLineAuthorMap(repoRoot, file, commitSha));
            }
        }));
    }

    for (auto& fut : futures) {
        auto [path, blame] = fut.get();
        result[path] = blame;
    }

    return result;
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
        report.policy.blocked = false;
        report.policy.threshold_blocked = false;
        report.policy.message = "No commits found in range.";
        return report;
    }

    // Fetch attribution notes for all commits — batched into single subprocess
    std::map<std::string, note::NoteReader::Result> ghostNotes;
    {
        BenchmarkTimer t("fetch attribution notes");
        ghostNotes = loadAttributionNotes(repoRoot, commitShas, cfg.gitai_fallback);
    }

    // A1: Cache diff-tree results per commit — single pass
    std::set<std::string> allFiles;
    std::map<std::string, std::vector<std::string>> commitFiles;
    {
        BenchmarkTimer t("libgit2 changed files");
        for (const auto& sha : commitShas) {
            auto files = git::Engine::changedFiles(repoRoot, sha);
            for (const auto& file : files) {
                allFiles.insert(file);
                commitFiles[sha].push_back(file);
            }
        }
    }

    // Blame cache (flat vector, not map) — parallelized
    std::map<std::string, git::BlameResult> blameCache;
    {
        BenchmarkTimer t("blame all files");
        std::vector<std::string> fileVec(allFiles.begin(), allFiles.end());
        size_t numThreads = std::max(1u, std::thread::hardware_concurrency());
        blameCache = blameFilesParallel(repoRoot, fileVec, "", numThreads);
    }

    // A2: Overlay cache per file — computed once, reused across commits
    std::map<std::string, FileAttribution> overlayCache;

    // Batch author lookup for all commits upfront
    std::map<std::string, std::string> authorCache;
    {
        BenchmarkTimer t("fetch all authors");
        std::set<std::string> shaSet(commitShas.begin(), commitShas.end());
        authorCache = getCommitAuthorsBatch(repoRoot, shaSet);
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
            cs.has_verified_note = git::Engine::noteExists(repoRoot, "refs/notes/ghost-verified", sha);

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
                if (config::IgnoreMatcher::matches(filePath, cfg.ignore)) continue;
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
    std::vector<std::string> commitShas = getCommitsInRange(repoRoot, range);
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

AuditReport Auditor::runFinalDiff(
    const std::string& repoRoot,
    const std::string& range,
    int thresholdOverride,
    bool jsonOutput,
    const std::string& configRef
) {
    initBenchmark();
    BenchmarkTimer totalTimer("runFinalDiff total");

    AuditReport report;
    report.json = jsonOutput;

    config::GhostConfig cfg = configRef.empty()
        ? config::GhostConfigReader::load(repoRoot)
        : config::GhostConfigReader::loadFromRef(repoRoot, configRef);

    if (!git::Ref::isSafeRange(range)) {
        report.summary = AuditSummary{};
        report.policy.passed = false;
        report.policy.blocked = true;
        report.policy.threshold_blocked = false;
        report.policy.message = "Invalid commit range.";
        return report;
    }

    std::string headRef = headFromRange(range);
    if (!git::Ref::isSafeCommitish(headRef)) headRef = "HEAD";
    std::string headSha = git::Engine::resolveCommit(repoRoot, headRef);
    if (headSha.empty()) headSha = headRef;

    git::DiffRanges ranges = git::Diff::getChangedRanges(repoRoot, range);
    std::vector<std::string> files;
    for (const auto& [file, lineRanges] : ranges.added) {
        if (!file.empty() && !lineRanges.empty() && !config::IgnoreMatcher::matches(file, cfg.ignore)) {
            files.push_back(file);
        }
    }

    if (files.empty()) {
        CommitSummary cs;
        cs.commit_sha = headSha;
        cs.author = git::Engine::commitAuthor(repoRoot, headSha);
        cs.total_lines = 0;
        cs.ai_lines = 0;
        cs.has_ghost_note = true;
        cs.has_verified_note = true;
        cs.primary_agent = "human";
        report.summary = Aggregator::aggregate({cs});
        report.policy = Policy::enforce(report.summary, cfg, thresholdOverride);
        report.policy.message = "Final diff has no added lines to enforce.";
        return report;
    }

    AttributionQuery query;
    query.repo_root = repoRoot;
    query.target_ref = headRef;
    query.config_ref = configRef;
    query.file_filter = files;
    query.line_filter = ranges.added;
    AttributionResult attribution = AttributionResolver::resolve(query);

    CommitSummary cs;
    cs.commit_sha = attribution.target_sha.empty() ? headSha : attribution.target_sha;
    cs.author = git::Engine::commitAuthor(repoRoot, cs.commit_sha);
    cs.total_lines = attribution.total_lines;
    cs.ai_lines = attribution.ai_lines;
    cs.has_ghost_note = true;
    cs.has_verified_note = true;
    cs.primary_agent = "human";
    cs.primary_model = "";

    std::map<std::string, int> entityLines;
    for (const auto& file : attribution.files) {
        FileAttribution finalFile;
        finalFile.file_path = file.path;
        finalFile.total_lines = file.total_lines;
        finalFile.ai_lines = file.ai_lines;
        for (const auto& line : file.lines) {
            LineAttribution outLine;
            outLine.line_number = line.line_number;
            outLine.commit_sha = line.commit_sha;
            outLine.is_ai = line.is_ai;
            outLine.session_id = line.session_id;
            outLine.agent = line.agent;
            outLine.model = line.model;
            finalFile.lines.push_back(outLine);
            if (line.is_ai) {
                entityLines[line.agent + "/" + line.model]++;
            }
        }
        if (finalFile.total_lines > 0) cs.files.push_back(finalFile);
    }

    int bestEntityCount = 0;
    for (const auto& [entity, count] : entityLines) {
        if (count <= bestEntityCount) continue;
        bestEntityCount = count;
        size_t slash = entity.find('/');
        cs.primary_agent = slash == std::string::npos ? entity : entity.substr(0, slash);
        cs.primary_model = slash == std::string::npos ? "" : entity.substr(slash + 1);
    }

    report.summary = Aggregator::aggregate({cs});
    report.policy = Policy::enforce(report.summary, cfg, thresholdOverride);
    if (report.policy.passed) {
        report.policy.message = "Final diff complies with policy.";
    } else if (!attribution.message.empty() && report.policy.message.empty()) {
        report.policy.message = attribution.message;
    }
    return report;
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

    if (!git::Ref::isSafeCommitish(target)) {
        report.policy.passed = true;
        report.policy.message = "Invalid commit reference: " + target;
        return report;
    }

    AttributionQuery query;
    query.repo_root = repoRoot;
    query.target_ref = target;
    query.config_ref = configRef;
    AttributionResult attribution = AttributionResolver::resolve(query);
    if (attribution.target_sha.empty()) {
        report.policy.passed = true;
        report.policy.message = attribution.message.empty()
            ? "Invalid commit reference: " + target
            : attribution.message;
        return report;
    }

    std::string sha = attribution.target_sha;
    std::set<std::string> changedFiles;
    for (const auto& df : git::Engine::changedFiles(repoRoot, sha)) {
        changedFiles.insert(df);
    }

    std::set<std::string> ghostNoteFiles;
    auto targetNotes = AttributionNotes::load(repoRoot, std::vector<std::string>{sha}, cfg.gitai_fallback);
    auto targetNoteIt = targetNotes.find(sha);
    if (targetNoteIt != targetNotes.end()) {
        for (const auto& entry : targetNoteIt->second.entries) {
            ghostNoteFiles.insert(entry.file_path);
        }
    }

    std::vector<FileBlameSummary> fileSummaries;
    int commitAiLines = 0;
    int commitTotalLines = 0;

    for (const auto& file : attribution.files) {
        if (changedFiles.count(file.path) > 0) {
            commitTotalLines += file.total_lines;
        }

        if (file.ai_lines == 0) continue;

        FileBlameSummary fbs;
        fbs.file_path = file.path;
        fbs.total_lines = file.total_lines;
        fbs.ai_lines = file.ai_lines;
        fbs.primary_author = file.primary_author;
        fbs.primary_entity = file.primary_entity.empty() ? "human" : file.primary_entity;
        fbs.in_commit = changedFiles.count(file.path) > 0 && ghostNoteFiles.count(file.path) > 0;

        std::map<std::string, int> commitAgentLines;
        for (const auto& line : file.lines) {
            if (line.commit_sha == sha && line.is_ai) {
                commitAiLines++;
                commitAgentLines[line.agent + "/" + line.model]++;
            }
        }
        for (const auto& entity : file.entities) {
            fbs.entities.push_back({entity.agent, entity.model, entity.lines});
        }

        int bestCommitEntityCount = 0;
        for (const auto& [key, count] : commitAgentLines) {
            if (count > bestCommitEntityCount) {
                bestCommitEntityCount = count;
                fbs.commit_entity = key;
            }
        }
        if (fbs.commit_entity.empty()) fbs.commit_entity = "human";

        fileSummaries.push_back(fbs);
    }

    auto sortByAiPct = [](const FileBlameSummary& a, const FileBlameSummary& b) {
        double pctA = a.total_lines > 0 ? (double)a.ai_lines / a.total_lines : 0;
        double pctB = b.total_lines > 0 ? (double)b.ai_lines / b.total_lines : 0;
        if (pctA != pctB) return pctA > pctB;
        return a.file_path < b.file_path;
    };
    std::sort(fileSummaries.begin(), fileSummaries.end(), sortByAiPct);

    report.summary.target_sha = sha;
    report.summary.files = fileSummaries;
    report.summary.total_lines = attribution.total_lines;
    report.summary.ai_lines = attribution.ai_lines;
    report.summary.commit_ai_lines = commitAiLines;
    report.summary.commit_total_lines = commitTotalLines;
    report.policy = Policy::enforce(
        audit::AuditSummary{std::vector<CommitSummary>(), attribution.total_lines, attribution.ai_lines},
        cfg,
        thresholdOverride
    );
    if (!attribution.message.empty() && report.policy.message.empty()) {
        report.policy.message = attribution.message;
    } else if (report.policy.passed && attribution.ai_lines == 0) {
        report.policy.message = attribution.message;
    }

    return report;
}


}
}
