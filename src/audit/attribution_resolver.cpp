#include "attribution_resolver.hpp"

#include "attribution_notes.hpp"
#include "blame_overlay.hpp"
#include "thread_pool.hpp"
#include "../config/ghost_config.hpp"
#include "../config/ignore_matcher.hpp"
#include "../git/blame.hpp"
#include "../git/engine.hpp"
#include "../git/path.hpp"
#include "../git/ref.hpp"

#include <algorithm>
#include <future>
#include <map>
#include <set>
#include <thread>

namespace ghost {
namespace audit {
namespace {

static std::map<std::string, git::BlameResult> blameFilesParallel(
    const std::string& repoRoot,
    const std::vector<std::string>& files,
    const std::string& commitSha
) {
    std::map<std::string, git::BlameResult> result;
    if (files.empty()) return result;

    size_t numThreads = std::max(1u, std::thread::hardware_concurrency());
    ghost::util::ThreadPool pool(numThreads);
    std::vector<std::future<std::pair<std::string, git::BlameResult>>> futures;
    futures.reserve(files.size());

    for (const auto& f : files) {
        futures.push_back(pool.enqueue([repoRoot, file = f, commitSha]() {
            return std::make_pair(file, git::Blame::getLineAuthorMap(repoRoot, file, commitSha));
        }));
    }

    for (auto& fut : futures) {
        auto [path, blame] = fut.get();
        result[path] = blame;
    }
    return result;
}

static std::vector<std::string> normalizeFiles(
    const std::string& repoRoot,
    const std::vector<std::string>& files,
    const std::vector<std::string>& ignore
) {
    std::set<std::string> unique;
    for (const auto& file : files) {
        std::string normalized = git::Path::normalizeRepoPathOrEmpty(file, repoRoot);
        if (normalized.empty()) continue;
        if (config::IgnoreMatcher::matches(normalized, ignore)) continue;
        unique.insert(normalized);
    }
    return std::vector<std::string>(unique.begin(), unique.end());
}

static std::vector<std::string> trackedFiles(
    const AttributionQuery& query,
    const config::GhostConfig& cfg,
    const std::string& targetSha
) {
    if (!query.file_filter.empty()) {
        return normalizeFiles(query.repo_root, query.file_filter, cfg.ignore);
    }

    std::vector<std::string> files;
    for (const auto& file : git::Engine::treeFiles(query.repo_root, targetSha)) {
        if (!file.empty() && !config::IgnoreMatcher::matches(file, cfg.ignore)) {
            files.push_back(file);
        }
    }
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    return files;
}

static std::map<std::string, note::LineRangeSet> normalizedLineFilter(const AttributionQuery& query) {
    std::map<std::string, note::LineRangeSet> result;
    for (const auto& [file, ranges] : query.line_filter) {
        std::string normalized = git::Path::normalizeRepoPathOrEmpty(file, query.repo_root);
        if (!normalized.empty()) result[normalized] = ranges;
    }
    return result;
}

static std::string bestKey(const std::map<std::string, int>& counts, const std::string& fallback) {
    std::string best = fallback;
    int bestCount = 0;
    for (const auto& [key, count] : counts) {
        if (count > bestCount || (count == bestCount && best != fallback && key < best)) {
            best = key;
            bestCount = count;
        }
    }
    return best;
}

static std::vector<AttributionEntity> sortedEntities(const std::map<std::string, int>& agentLines) {
    std::vector<std::pair<std::string, int>> sorted(agentLines.begin(), agentLines.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
    });

    std::vector<AttributionEntity> entities;
    for (const auto& [key, count] : sorted) {
        AttributionEntity entity;
        size_t slash = key.find('/');
        entity.agent = slash == std::string::npos ? key : key.substr(0, slash);
        entity.model = slash == std::string::npos ? "unknown" : key.substr(slash + 1);
        entity.lines = count;
        entities.push_back(entity);
    }
    return entities;
}

static void sortFiles(std::vector<AttributionFile>& files) {
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        bool aAi = a.ai_lines > 0;
        bool bAi = b.ai_lines > 0;
        if (aAi != bAi) return aAi > bAi;
        double aPct = a.total_lines > 0 ? static_cast<double>(a.ai_lines) / a.total_lines : 0.0;
        double bPct = b.total_lines > 0 ? static_cast<double>(b.ai_lines) / b.total_lines : 0.0;
        if (aPct != bPct) return aPct > bPct;
        return a.path < b.path;
    });
}

}

AttributionResult AttributionResolver::resolve(const AttributionQuery& query) {
    AttributionResult result;

    if (query.repo_root.empty()) {
        result.message = "Not in a git repository.";
        return result;
    }

    std::string targetRef = query.target_ref.empty() ? "HEAD" : query.target_ref;
    if (!git::Ref::isSafeCommitish(targetRef)) {
        result.message = "Invalid commit reference: " + targetRef;
        return result;
    }

    result.target_sha = git::Engine::resolveCommit(query.repo_root, targetRef);
    if (result.target_sha.empty()) {
        result.message = "Invalid commit reference: " + targetRef;
        return result;
    }

    config::GhostConfig cfg = query.config_ref.empty()
        ? config::GhostConfigReader::load(query.repo_root)
        : config::GhostConfigReader::loadFromRef(query.repo_root, query.config_ref);

    std::vector<std::string> files = trackedFiles(query, cfg, result.target_sha);
    if (files.empty()) {
        result.message = "Current codebase has no tracked files.";
        return result;
    }

    auto blameCache = blameFilesParallel(query.repo_root, files, result.target_sha);
    std::set<std::string> allShas;
    bool sawBlame = false;
    for (const auto& [_, blame] : blameCache) {
        if (!blame.empty()) sawBlame = true;
        for (const auto& sha : blame.lines) {
            if (!sha.empty()) allShas.insert(sha);
        }
    }
    if (!sawBlame) {
        result.message = "Blame could not resolve tracked files at " + result.target_sha.substr(0, 8) + ".";
        return result;
    }
    allShas.insert(result.target_sha);

    std::vector<std::string> shaVec(allShas.begin(), allShas.end());
    auto notes = AttributionNotes::load(query.repo_root, shaVec, cfg.gitai_fallback);
    auto authors = git::Engine::commitAuthors(query.repo_root, shaVec);
    auto lineFilter = normalizedLineFilter(query);
    bool hasLineFilter = !lineFilter.empty();

    for (const auto& file : files) {
        auto blameIt = blameCache.find(file);
        if (blameIt == blameCache.end() || blameIt->second.empty()) continue;

        auto overlay = BlameOverlay::overlay(file, blameIt->second, notes);
        AttributionFile attributedFile;
        attributedFile.path = file;

        std::map<std::string, int> authorLines;
        std::map<std::string, int> agentLines;
        auto filterIt = lineFilter.find(file);

        for (const auto& line : overlay.lines) {
            if (hasLineFilter) {
                if (filterIt == lineFilter.end() || !filterIt->second.contains(line.line_number)) {
                    continue;
                }
            }

            AttributionLine attributedLine;
            attributedLine.line_number = line.line_number;
            attributedLine.source_line_number =
                static_cast<size_t>(line.line_number - 1) < blameIt->second.source_lines.size()
                    ? blameIt->second.source_lines[line.line_number - 1]
                    : line.line_number;
            attributedLine.commit_sha = line.commit_sha;
            attributedLine.is_ai = line.is_ai;
            attributedLine.session_id = line.session_id;
            attributedLine.agent = line.agent;
            attributedLine.model = line.model;
            auto authorIt = authors.find(line.commit_sha);
            attributedLine.author = authorIt == authors.end() ? "unknown" : authorIt->second;

            attributedFile.lines.push_back(attributedLine);
            attributedFile.total_lines++;
            result.total_lines++;
            authorLines[attributedLine.author]++;

            if (attributedLine.is_ai) {
                attributedFile.ai_lines++;
                result.ai_lines++;
                agentLines[attributedLine.agent + "/" + attributedLine.model]++;
            }
        }

        if (attributedFile.total_lines == 0) continue;
        attributedFile.primary_author = bestKey(authorLines, "unknown");
        attributedFile.primary_entity = agentLines.empty() ? "human" : bestKey(agentLines, "human");
        attributedFile.entities = sortedEntities(agentLines);
        result.files.push_back(attributedFile);
    }

    sortFiles(result.files);
    if (result.total_lines == 0 && hasLineFilter) {
        result.message = "Final diff has no live lines to enforce.";
    } else if (result.total_lines == 0) {
        result.message = "Blame could not resolve tracked files at " + result.target_sha.substr(0, 8) + ".";
    } else if (result.ai_lines == 0) {
        result.message = "No AI-attributed lines found in the current codebase.";
    } else {
        result.message = "Current codebase attribution resolved.";
    }
    return result;
}

}
}
