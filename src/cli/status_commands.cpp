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
    (void)verbose;
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

    std::cout << Style::header("status");
    std::cout << "  " << Style::padRight(Style::dim("repo"), 14) << Style::glow(repoRoot) << "\n";
    std::cout << "  " << Style::padRight(Style::dim("branch"), 14) << Style::violet(branch) << "\n\n";

    auto cfg = config::GhostConfigReader::load(repoRoot);
    std::cout << Style::subHeader("Policy");
    std::cout << "  " << Style::padRight(Style::dim("mode"), 14) << Style::glow(cfg.mode.empty() ? "custom" : cfg.mode) << "\n";
    std::cout << "  " << Style::padRight(Style::dim("required"), 14) << (cfg.required ? Style::success("yes") : Style::dim("no")) << "\n";
    std::cout << "  " << Style::padRight(Style::dim("threshold"), 14) << Style::glow(std::to_string(cfg.threshold) + "%") << "\n";
    std::cout << "  " << Style::padRight(Style::dim("action"), 14) << Style::glow(cfg.on_exceed) << "\n";
    if (!cfg.ignore.empty()) {
        std::cout << "  " << Style::padRight(Style::dim("ignored"), 14) << Style::dim(cfg.ignore[0]);
        for (size_t i = 1; i < cfg.ignore.size(); ++i) {
            std::cout << Style::dim(", " + cfg.ignore[i]);
        }
        std::cout << "\n";
    }

    bool postCommit = util::Files::exists(repoRoot + "/.git/hooks/post-commit");
    bool prePush = util::Files::exists(repoRoot + "/.git/hooks/pre-push");
    std::string notesPush = git::Command::capture(repoRoot, {"config", "--get-all", "remote.origin.push"});
    bool notesConfigured = notesPush.find("refs/notes/ghost") != std::string::npos;
    bool localReady = postCommit && prePush && notesConfigured;

    std::cout << "\n" << Style::subHeader("Setup");
    std::cout << "  " << Style::padRight(Style::dim("local"), 14)
              << (localReady ? Style::success("ready") : Style::warning("needs setup"))
              << Style::dim(localReady ? "  capture and push checks are configured" : "  run ghost init --contributor")
              << "\n";
    std::cout << "  " << Style::padRight(Style::dim("notes"), 14)
              << (notesConfigured ? Style::success("configured") : Style::warning("not configured"))
              << Style::dim("  used by audit and PR checks")
              << "\n";

    auto stagedFiles = git::Diff::getChangedFiles("--cached");
    auto unstagedFiles = git::Diff::getChangedFiles("");
    std::cout << "\n" << Style::subHeader("Working Tree");
    std::cout << "  " << Style::padRight(Style::dim("staged"), 14) << Style::glow(std::to_string(stagedFiles.size()) + " files")
              << Style::dim("  checked by ghost check") << "\n";
    std::cout << "  " << Style::padRight(Style::dim("unstaged"), 14) << Style::glow(std::to_string(unstagedFiles.size()) + " files")
              << Style::dim("  stage before checking") << "\n";

    std::cout << "\n" << Style::subHeader("Pending Attribution");
    std::cout << "  " << Style::dim("Captured AI edits that will attach to the next commit.") << "\n";

    {
        std::vector<persist::Session> sessions = SessionSummary::loadPending(repoRoot);

        if (sessions.empty()) {
            std::cout << "  " << Style::padRight(Style::dim("state"), 14) << Style::dim("none captured") << "\n";
        } else {
            std::sort(sessions.begin(), sessions.end(),
                [](const auto& a, const auto& b) { return a.ts_start > b.ts_start; });

            int totalAiAdditions = 0;
            int totalAiDeletions = 0;
            for (const auto& s : sessions) {
                totalAiAdditions += s.additions;
                totalAiDeletions += s.deletions;
            }

            std::cout << "  " << Style::padRight(Style::dim("sessions"), 14) << Style::glow(std::to_string(sessions.size())) << "\n";
            std::cout << "  " << Style::padRight(Style::dim("captured"), 14)
                      << Style::success("+" + std::to_string(totalAiAdditions))
                      << Style::dim(" / ")
                      << Style::warning("-" + std::to_string(totalAiDeletions)) << "\n";

            if (totalAiAdditions > 0 || totalAiDeletions > 0) {
                int total = totalAiAdditions + totalAiDeletions;
                int barWidth = 24;
                int aiChars = (total > 0) ? (totalAiAdditions * barWidth) / total : 0;
                std::string bar;
                for (int i = 0; i < barWidth; ++i) {
                    bar += (i < aiChars) ? "█" : "·";
                }
                int pct = (total > 0) ? std::min((totalAiAdditions * 100) / total, 100) : 0;
                std::cout << "  " << Style::padRight(Style::dim("mix"), 14)
                          << Style::violet(bar)
                          << "  " << Style::glow(std::to_string(pct) + "% additions") << "\n";
            }

            std::cout << "\n";
            for (const auto& s : sessions) {
                std::string agentModel = s.agent + "/" + s.model;
                auto files = SessionSummary::files(s.json_data, repoRoot);
                std::cout << "  " << Style::padRight(Style::dim(timeAgo(s.ts_start)), 14)
                          << Style::padRight(Style::success("+" + std::to_string(s.additions)) + Style::dim("/") + Style::warning("-" + std::to_string(s.deletions)), 12)
                          << Style::padRight(Style::glow(agentModel), 34)
                          << Style::dim(std::to_string(files.size()) + " file" + (files.size() == 1 ? "" : "s")) << "\n";
                for (size_t i = 0; i < std::min<size_t>(files.size(), 3); ++i) {
                    std::cout << "    " << Style::dim(files[i]) << "\n";
                }
                if (files.size() > 3) {
                    std::cout << "    " << Style::dim("+" + std::to_string(files.size() - 3) + " more") << "\n";
                }
            }
        }
    }

    std::string headSha = git::Repo::getHead();
    std::cout << "\n" << Style::subHeader("HEAD Attribution");
    if (headSha.empty()) {
        std::cout << "  " << Style::padRight(Style::dim("state"), 14) << Style::dim("no commits yet") << "\n\n";
        return kExitOk;
    }
    std::string note = git::Notes::show("refs/notes/ghost", headSha);
    std::cout << "  " << Style::padRight(Style::dim("commit"), 14) << Style::violet(headSha.substr(0, 8)) << "\n";
    if (!note.empty()) {
        auto parsed = note::NoteReader::parse(note);
        if (parsed.success) {
            std::set<std::string> files;
            int aiLines = 0;
            for (const auto& e : parsed.entries) {
                files.insert(e.file_path);
                aiLines += static_cast<int>(e.ranges.lineCount());
            }
            std::cout << "  " << Style::padRight(Style::dim("state"), 14) << Style::success("attributed") << "\n";
            std::cout << "  " << Style::padRight(Style::dim("ai lines"), 14) << Style::glow(std::to_string(aiLines)) << "\n";
            std::cout << "  " << Style::padRight(Style::dim("files"), 14) << Style::glow(std::to_string(files.size())) << "\n";
        } else {
            std::cout << "  " << Style::padRight(Style::dim("state"), 14) << Style::warning("unreadable attribution") << "\n";
        }
    } else {
        std::cout << "  " << Style::padRight(Style::dim("state"), 14) << Style::dim("no attribution") << "\n";
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
        std::cout << "  " << Style::dim("Staged changes only. Stage files before checking attribution.") << "\n\n";
        if (ignoredStagedFiles > 0) {
            std::cout << "  " << Style::warning("Only ignored staged changes") << "\n";
            std::cout << "  " << Style::dim(std::to_string(ignoredStagedFiles) + " staged file(s) matched ghost.yml ignore patterns.") << "\n";
        } else {
            std::cout << "  " << Style::warning("No staged changes to check") << "\n";
            std::cout << "  " << Style::dim("Run 'git add <files>' first, then 'ghost check'.") << "\n";
        }
        std::cout << "  " << Style::dim("Use 'ghost status' to see pending captured attribution.") << "\n\n";
        return kExitOk;
    }

    std::vector<persist::Session> uncommittedSessions = SessionSummary::loadPending(repoRoot);
    std::sort(uncommittedSessions.begin(), uncommittedSessions.end(),
        [](const auto& a, const auto& b) { return a.ts_start > b.ts_start; });

    std::string headSha = git::Repo::getHead();
    std::map<std::string, note::NoteReader::Result> ghostNotes;
    if (!headSha.empty()) {
        std::string rawNote = git::Notes::show("refs/notes/ghost", headSha);
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
        std::cout << "  " << Style::dim("Staged changes only. Commit attribution is verified after commit.") << "\n\n";

        auto v = Style::violet;
        auto d = Style::dim;

        std::cout << Style::subHeader("Summary");
        std::cout << "  " << Style::padRight(Style::dim("files"), 18) << Style::glow(std::to_string(stagedFiles.size())) << "\n";
        if (ignoredStagedFiles > 0) {
            std::cout << "  " << Style::padRight(Style::dim("ignored"), 18) << Style::dim(std::to_string(ignoredStagedFiles)) << "\n";
        }
        std::cout << "  " << Style::padRight(Style::dim("sessions"), 18) << Style::glow(std::to_string(uncommittedSessions.size())) << "\n";
        std::cout << "  " << Style::padRight(Style::dim("additions"), 18) << Style::success("+" + std::to_string(totalAdditions)) << "\n";
        std::cout << "  " << Style::padRight(Style::dim("ai predicted"), 18) << (predictedAiAdditions > 0 ? Style::success("+" + std::to_string(predictedAiAdditions)) : d("0")) << "\n";
        std::cout << "  " << Style::padRight(Style::dim("ai share"), 18) << v(std::to_string(static_cast<int>(aiPercent)) + "%") << "\n";

        std::cout << "\n" << Style::subHeader("Files");
        for (const auto& p : predictions) {
            int pct = p.additions > 0 ? std::min((p.predictedAiAdditions * 100) / p.additions, 100) : 0;
            std::cout << "  " << Style::padRight(Style::blue(p.path), 34);
            std::cout << Style::padRight(std::to_string(p.additions) + "+ " + std::to_string(p.deletions) + "-", 10);
            std::cout << Style::progressBar(pct, 100, 8) << " ";
            std::cout << d(p.reason);
            std::cout << "\n";
        }

        std::cout << "\n" << Style::subHeader("Policy");
        std::cout << "  " << Style::padRight(d("threshold"), 18) << d(std::to_string(cfg.threshold) + "%") << "\n";
        std::cout << "  " << Style::padRight(d("result"), 18);
        if (wouldPass) {
            std::cout << Style::success(statusMsg);
        } else {
            std::cout << Style::error(statusMsg);
        }
        std::cout << "\n";
        std::cout << "  " << d("Run 'ghost audit' after committing for durable attribution.") << "\n\n";
    }

    return wouldPass ? kExitOk : kExitBlocked;
}

}
}
