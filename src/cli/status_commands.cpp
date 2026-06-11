#include "status_commands.hpp"
#include "args.hpp"
#include "exit_codes.hpp"
#include "session_summary.hpp"

#include "config/ghost_config.hpp"
#include "config/ignore_matcher.hpp"
#include "git/command.hpp"
#include "git/diff.hpp"
#include "git/notes.hpp"
#include "git/path.hpp"
#include "git/ref.hpp"
#include "git/repo.hpp"
#include "note/reader.hpp"
#include "output/layout.hpp"
#include "output/style.hpp"
#include "persist/db.hpp"
#include "util/files.hpp"
#include "util/json.hpp"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace ghost {
namespace cli {
namespace {

static void logVerbose(bool verbose, const std::string& msg) {
    if (verbose) {
        std::cerr << output::Style::dim("[verbose] " + msg) << "\n";
    }
}

static std::string timeAgo(time_t ts) {
    time_t now = std::time(nullptr);
    double diff = difftime(now, ts);
    if (diff < 0) diff = 0;
    if (diff < 5) return "just now";
    if (diff < 60) return std::to_string(static_cast<int>(diff)) + " secs ago";
    if (diff < 3600) return std::to_string(static_cast<int>(diff / 60)) + " mins ago";
    if (diff < 86400) return std::to_string(static_cast<int>(diff / 3600)) + " hrs ago";
    return std::to_string(static_cast<int>(diff / 86400)) + " days ago";
}

}

int status(int argc, char* argv[], bool verbose) {
    using namespace ghost::output;

    Args args(argc, argv);
    std::string repoRoot = git::Repo::getRoot();
    std::string repoArg = args.getValue("--repo");
    if (!repoArg.empty()) repoRoot = repoArg;
    if (repoRoot.empty() || !util::Files::exists(repoRoot + "/.git")) {
        std::cerr << Style::error("Not in a git repository") << "\n";
        return kExitNotInRepo;
    }
    std::error_code cwdEc;
    std::filesystem::current_path(repoRoot, cwdEc);

    std::string branch = git::Command::capture(repoRoot, {"branch", "--show-current"});
    if (branch.empty()) branch = "detached";

    auto cfg = config::GhostConfigReader::load(repoRoot);

    bool postCommit = util::Files::exists(repoRoot + "/.git/hooks/post-commit");
    bool prePush = util::Files::exists(repoRoot + "/.git/hooks/pre-push");
    std::string notesPush = git::Command::capture(repoRoot, {"config", "--get-all", "remote.origin.push"});
    bool notesConfigured = notesPush.find("refs/notes/ghost") != std::string::npos;
    bool localReady = postCommit && prePush && notesConfigured;

    auto stagedFiles = git::Diff::getChangedFiles("--cached");
    auto unstagedFiles = git::Diff::getChangedFiles("");
    std::vector<persist::Session> sessions = SessionSummary::loadPending(repoRoot);
    std::sort(sessions.begin(), sessions.end(),
        [](const auto& a, const auto& b) { return a.ts_start > b.ts_start; });

    std::string repoName = std::filesystem::path(repoRoot).filename().string();
    if (repoName.empty()) repoName = repoRoot;
    std::string readiness = localReady ? Style::success("ready") : Style::warning("needs setup");
    std::string mode = cfg.mode.empty() ? "custom" : cfg.mode;
    std::string required = cfg.required ? "required" : "optional";

    std::cout << Style::header("status");
    std::cout << "  " << readiness << "  " << Style::glow(repoName)
              << Style::dim(" on ") << Style::violet(branch) << "\n";
    std::cout << "  " << Style::dim("policy ")
              << Style::muted(mode + " · " + required + " · " + std::to_string(cfg.threshold) + "% " + cfg.on_exceed)
              << "\n";
    if (!localReady) {
        std::string action = (!postCommit || !prePush)
            ? "run ghost init --contributor"
            : "configure Ghost note push refs";
        std::cout << "  " << Style::dim("action ") << Style::warning(action) << "\n";
    }
    std::cout << "\n";

    std::cout << Style::subHeader("Worktree");
    std::cout << "  " << Style::glow(std::to_string(stagedFiles.size()))
              << Style::dim(" staged · ")
              << Style::glow(std::to_string(unstagedFiles.size()))
              << Style::dim(" unstaged") << "\n";
    if (stagedFiles.empty()) {
        std::cout << "  " << Style::dim("stage changes before running ghost check") << "\n";
    } else {
        std::cout << "  " << Style::dim("ghost check will preview staged attribution") << "\n";
    }

    std::cout << "\n" << Style::subHeader("Pending Attribution");
    if (sessions.empty()) {
        std::cout << "  " << Style::dim("none captured") << "\n";
    } else {
        int totalAiAdditions = 0;
        int totalAiDeletions = 0;
        for (const auto& s : sessions) {
            totalAiAdditions += s.additions;
            totalAiDeletions += s.deletions;
        }

        std::cout << "  " << Style::glow(std::to_string(sessions.size()))
                  << Style::dim(" sessions · ")
                  << Style::success("+" + std::to_string(totalAiAdditions))
                  << Style::dim(" / ")
                  << Style::warning("-" + std::to_string(totalAiDeletions)) << "\n\n";

        size_t width = Layout::contentWidth();
        size_t sourceCol = width >= 100 ? 34 : 26;
        size_t fileCol = width > sourceCol + 34 ? width - sourceCol - 34 : 28;
        std::cout << "  " << Layout::fitCell(Style::dim("When"), 12)
                  << Layout::fitCell(Style::dim("Lines"), 10)
                  << Layout::fitCell(Style::dim("Source"), sourceCol)
                  << Style::dim("Files") << "\n";
        for (const auto& s : sessions) {
            std::string agentModel = s.agent + "/" + s.model;
            auto files = SessionSummary::files(s.json_data, repoRoot);
            std::string fileSummary = files.empty() ? "no files" : files.front();
            if (files.size() > 1) fileSummary += " +" + std::to_string(files.size() - 1);
            std::cout << "  " << Layout::fitCell(Style::dim(timeAgo(s.ts_start)), 12)
                      << Layout::fitCell(Style::success("+" + std::to_string(s.additions)) + Style::dim("/") + Style::warning("-" + std::to_string(s.deletions)), 10)
                      << Layout::fitCell(Style::glow(agentModel), sourceCol)
                      << Style::dim(Layout::ellipsizeMiddle(fileSummary, fileCol)) << "\n";
            if (files.size() > 1 && verbose) {
                for (size_t i = 1; i < std::min<size_t>(files.size(), 4); ++i) {
                    std::cout << "    " << Style::dim(files[i]) << "\n";
                }
            }
        }
    }

    std::string headSha = git::Repo::getHead();
    std::cout << "\n" << Style::subHeader("HEAD");
    if (headSha.empty()) {
        std::cout << "  " << Style::dim("no commits yet") << "\n\n";
        return kExitOk;
    }
    std::string note = git::Notes::show(repoRoot, "refs/notes/ghost", headSha);
    if (!note.empty()) {
        auto parsed = note::NoteReader::parse(note);
        if (parsed.success) {
            std::set<std::string> files;
            int aiLines = 0;
            for (const auto& e : parsed.entries) {
                files.insert(e.file_path);
                aiLines += static_cast<int>(e.ranges.lineCount());
            }
            std::cout << "  " << Style::violet(headSha.substr(0, 8))
                      << Style::dim(" · ")
                      << Style::glow(std::to_string(aiLines) + " AI lines")
                      << Style::dim(" across " + std::to_string(files.size()) + " file" + (files.size() == 1 ? "" : "s")) << "\n";
        } else {
            std::cout << "  " << Style::violet(headSha.substr(0, 8)) << Style::dim(" · ")
                      << Style::warning("unreadable attribution note") << "\n";
        }
    } else {
        std::cout << "  " << Style::violet(headSha.substr(0, 8)) << Style::dim(" · no attribution note") << "\n";
    }

    if (verbose) {
        std::cout << "\n" << Style::subHeader("Diagnostics");
        std::cout << Layout::keyValue("repo path", Style::muted(repoRoot), 14);
        std::cout << Layout::keyValue("post-commit", postCommit ? Style::success("installed") : Style::warning("missing"), 14);
        std::cout << Layout::keyValue("pre-push", prePush ? Style::success("installed") : Style::warning("missing"), 14);
        std::cout << Layout::keyValue("notes push", notesConfigured ? Style::success("configured") : Style::warning("missing"), 14);
        if (!cfg.ignore.empty()) {
            std::string ignored;
            for (size_t i = 0; i < cfg.ignore.size(); ++i) {
                if (i > 0) ignored += ", ";
                ignored += cfg.ignore[i];
            }
            std::cout << Layout::keyValue("ignored", Style::muted(ignored), 14);
        }
    }

    std::cout << "\n";
    return kExitOk;
}

int check(int argc, char* argv[], bool verbose) {
    Args args(argc, argv);
    std::string repoRoot = git::Repo::getRoot();
    if (repoRoot.empty()) {
        std::cerr << output::Style::error("Not in a git repository") << "\n";
        return kExitNotInRepo;
    }
    std::error_code cwdEc;
    std::filesystem::current_path(repoRoot, cwdEc);

    bool jsonOutput = args.hasAnyFlag({"--json", "-j"});
    std::string configRef = args.getValue("--config-ref");
    logVerbose(verbose, "checking staged changes");
    if (!configRef.empty() && !git::Ref::isSafeConfigRef(configRef)) {
        std::cerr << output::Style::error("Invalid config ref") << "\n";
        return kExitError;
    }

    using namespace ghost::output;

    auto cfg = configRef.empty()
        ? config::GhostConfigReader::load(repoRoot)
        : config::GhostConfigReader::loadFromRef(repoRoot, configRef);

    auto stagedFiles = git::Diff::getChangedFiles("--cached");
    auto stagedRanges = git::Diff::getChangedRanges(repoRoot, "--cached");
    size_t ignoredStagedFiles = 0;
    stagedFiles.erase(
        std::remove_if(stagedFiles.begin(), stagedFiles.end(),
            [&](const auto& file) {
                bool ignored = config::IgnoreMatcher::matches(file.path, cfg.ignore);
                if (ignored) ignoredStagedFiles++;
                return ignored;
            }),
        stagedFiles.end()
    );
    if (stagedFiles.empty()) {
        if (jsonOutput) {
            std::cout << "{\n";
            std::cout << "  \"scope\": \"staged_changes\",\n";
            std::cout << "  \"staged_files\": 0,\n";
            std::cout << "  \"ignored_staged_files\": " << ignoredStagedFiles << ",\n";
            std::cout << "  \"total_additions\": 0,\n";
            std::cout << "  \"predicted_ai_additions\": 0,\n";
            std::cout << "  \"predicted_ai_percent\": 0,\n";
            std::cout << "  \"would_pass\": true,\n";
            std::cout << "  \"status\": \"" << (ignoredStagedFiles > 0 ? "ONLY_IGNORED_STAGED_CHANGES" : "NO_STAGED_CHANGES") << "\",\n";
            std::cout << "  \"message\": \"" << (ignoredStagedFiles > 0 ? "All staged changes are ignored by ghost.yml." : "No staged changes to check.") << "\"\n";
            std::cout << "}\n";
            return kExitOk;
        }
        std::cout << Style::header("check");
        if (ignoredStagedFiles > 0) {
            std::cout << "  " << Style::warning("Only ignored staged changes") << "\n";
            std::cout << "  " << Style::dim(std::to_string(ignoredStagedFiles) + " staged file(s) matched ghost.yml ignore patterns.") << "\n\n";
        } else {
            std::cout << "  " << Style::warning("No staged changes") << "\n";
            std::cout << "  " << Style::dim("Run git add <files>, then ghost check.") << "\n\n";
        }
        return kExitOk;
    }

    std::vector<persist::Session> uncommittedSessions = SessionSummary::loadPending(repoRoot);
    std::sort(uncommittedSessions.begin(), uncommittedSessions.end(),
        [](const auto& a, const auto& b) { return a.ts_start > b.ts_start; });

    std::string headSha = git::Repo::getHead();
    std::map<std::string, note::NoteReader::Result> ghostNotes;
    if (!headSha.empty()) {
        std::string rawNote = git::Notes::show(repoRoot, "refs/notes/ghost", headSha);
        if (!rawNote.empty()) {
            ghostNotes[headSha] = note::NoteReader::parse(rawNote);
        }
    }

    int totalAdditions = 0;
    int predictedAiAdditions = 0;

    struct FilePrediction {
        std::string path;
        int additions;
        int deletions;
        int predictedAiAdditions;
        std::string reason;
        std::string basis;
    };
    std::vector<FilePrediction> predictions;

    for (const auto& df : stagedFiles) {
        FilePrediction pred;
        pred.path = df.path;
        pred.additions = df.additions;
        pred.deletions = df.deletions;
        totalAdditions += df.additions;

        note::LineRangeSet sessionAttributedRanges;
        const persist::Session* firstMatchingSession = nullptr;
        auto stagedRangeIt = stagedRanges.added.find(git::Path::normalizeRepoPathOrEmpty(df.path, repoRoot));
        for (const auto& session : uncommittedSessions) {
            auto ranges = SessionSummary::rangesForFile(session.json_data, df.path, repoRoot);
            if (ranges.empty()) continue;
            if (stagedRangeIt != stagedRanges.added.end()) {
                ranges = ranges.intersect(stagedRangeIt->second);
            }
            if (ranges.empty()) continue;
            sessionAttributedRanges = sessionAttributedRanges.unite(ranges);
            if (firstMatchingSession == nullptr) {
                firstMatchingSession = &session;
            }
        }

        if (!sessionAttributedRanges.empty() && firstMatchingSession != nullptr) {
            pred.predictedAiAdditions = static_cast<int>(sessionAttributedRanges.lineCount());
            pred.reason = "captured by " + firstMatchingSession->agent + "/" + firstMatchingSession->model;
            pred.basis = "uncommitted_session";
        } else {
            if (ghostNotes.count(headSha)) {
                const auto& noteResult = ghostNotes[headSha];
                bool hasAiHistory = false;
                for (const auto& entry : noteResult.entries) {
                    if (entry.file_path == df.path) {
                        hasAiHistory = true;
                        break;
                    }
                }
                if (hasAiHistory) {
                    pred.predictedAiAdditions = df.additions;
                    pred.reason = "continues existing AI-attributed file";
                    pred.basis = "head_note_history";
                } else {
                    pred.predictedAiAdditions = 0;
                    pred.reason = "no captured AI attribution";
                    pred.basis = "none";
                }
            } else {
                pred.predictedAiAdditions = 0;
                pred.reason = "no captured AI attribution";
                pred.basis = "none";
            }
        }
        predictedAiAdditions += pred.predictedAiAdditions;
        predictions.push_back(pred);
    }

    double aiPercent = totalAdditions > 0 ? (predictedAiAdditions * 100.0) / totalAdditions : 0.0;

    bool wouldPass = true;
    std::string statusMsg = "WOULD PASS";
    if (cfg.threshold > 0 && aiPercent > cfg.threshold) {
        if (cfg.on_exceed == "block") {
            wouldPass = false;
            statusMsg = "WOULD FAIL (exceeds threshold)";
        } else if (cfg.on_exceed == "warn") {
            statusMsg = "WOULD WARN (exceeds threshold)";
        }
    }

    if (jsonOutput) {
        std::cout << "{\n";
        std::cout << "  \"scope\": \"staged_changes\",\n";
        std::cout << "  \"basis\": \"uncommitted_sessions_then_head_notes\",\n";
        std::cout << "  \"staged_files\": " << stagedFiles.size() << ",\n";
        std::cout << "  \"ignored_staged_files\": " << ignoredStagedFiles << ",\n";
        std::cout << "  \"uncommitted_sessions\": " << uncommittedSessions.size() << ",\n";
        std::cout << "  \"total_additions\": " << totalAdditions << ",\n";
        std::cout << "  \"predicted_ai_additions\": " << predictedAiAdditions << ",\n";
        std::cout << "  \"predicted_ai_percent\": " << aiPercent << ",\n";
        std::cout << "  \"threshold\": " << cfg.threshold << ",\n";
        std::cout << "  \"would_pass\": " << (wouldPass ? "true" : "false") << ",\n";
        std::cout << "  \"status\": \"" << statusMsg << "\",\n";
        std::cout << "  \"files\": [\n";
        for (size_t i = 0; i < predictions.size(); ++i) {
            const auto& p = predictions[i];
            std::cout << "    {\"path\": \"" << util::Json::escape(p.path) << "\", ";
            std::cout << "\"additions\": " << p.additions << ", ";
            std::cout << "\"deletions\": " << p.deletions << ", ";
            std::cout << "\"predicted_ai_additions\": " << p.predictedAiAdditions << ", ";
            std::cout << "\"basis\": \"" << util::Json::escape(p.basis) << "\", ";
            std::cout << "\"reason\": \"" << util::Json::escape(p.reason) << "\"}";
            if (i + 1 < predictions.size()) std::cout << ",";
            std::cout << "\n";
        }
        std::cout << "  ]\n";
        std::cout << "}\n";
    } else {
        std::cout << Style::header("check");
        std::string verdict = wouldPass ? Style::success(statusMsg) : Style::error(statusMsg);
        std::cout << "  " << verdict
                  << Style::dim("  ")
                  << Style::violet(std::to_string(static_cast<int>(aiPercent)) + "% AI")
                  << Style::dim("  ")
                  << Style::glow(std::to_string(predictedAiAdditions) + " / " + std::to_string(totalAdditions) + " staged additions")
                  << Style::dim("  threshold ")
                  << Style::muted(std::to_string(cfg.threshold) + "% " + cfg.on_exceed)
                  << "\n";
        if (ignoredStagedFiles > 0) {
            std::cout << "  " << Style::dim(std::to_string(ignoredStagedFiles) + " ignored staged file(s)") << "\n";
        }
        if (uncommittedSessions.empty() && predictedAiAdditions == 0) {
            std::cout << "  " << Style::dim("no pending captured attribution affected these staged changes") << "\n";
        }

        std::cout << "\n" << Style::subHeader("Files");
        size_t width = Layout::contentWidth();
        size_t pathCol = width >= 100 ? 42 : 30;
        size_t changeCol = 10;
        size_t shareCol = 10;
        size_t reasonCol = width > pathCol + changeCol + shareCol + 8
            ? width - pathCol - changeCol - shareCol - 8
            : 24;
        std::cout << "  " << Layout::fitCell(Style::dim("File"), pathCol)
                  << Layout::fitCell(Style::dim("Change"), changeCol)
                  << Layout::fitCell(Style::dim("AI"), shareCol)
                  << Style::dim("Basis") << "\n";
        for (const auto& p : predictions) {
            int pct = p.additions > 0 ? std::min((p.predictedAiAdditions * 100) / p.additions, 100) : 0;
            std::string change = "+" + std::to_string(p.additions) + " -" + std::to_string(p.deletions);
            std::cout << "  " << Layout::fitCell(Style::blue(p.path), pathCol)
                      << Layout::fitCell(Style::muted(change), changeCol)
                      << Layout::fitCell(Style::violet(std::to_string(pct) + "%"), shareCol)
                      << Style::dim(Layout::ellipsizeMiddle(p.reason, reasonCol));
            std::cout << "\n";
        }

        std::cout << "\n";
        std::cout << "  " << Style::dim("commit, then run ghost audit for durable attribution") << "\n\n";
    }

    return wouldPass ? kExitOk : kExitBlocked;
}

}
}
