#include "inspection_commands.hpp"
#include "commands.hpp"
#include "exit_codes.hpp"
#include "audit/auditor.hpp"
#include "audit/blame_overlay.hpp"
#include "config/ghost_config.hpp"
#include "git/blame.hpp"
#include "git/notes.hpp"
#include "git/repo.hpp"
#include "note/gitai_reader.hpp"
#include "note/reader.hpp"
#include "output/style.hpp"
#include "util/json.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace ghost {
namespace cli {

static bool hasFlag(int argc, char* argv[], const std::string& flag) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == flag) return true;
    }
    return false;
}

static void logVerbose(bool verbose, const std::string& msg) {
    if (verbose) {
        std::cerr << output::Style::dim("[verbose] " + msg) << "\n";
    }
}

int show(int argc, char* argv[], bool verbose) {
    if (argc < 3 || std::string(argv[2])[0] == '-') {
        CommandRegistry::printHelp("show");
        return kExitError;
    }
    std::string commitSha = argv[2];
    logVerbose(verbose, "showing Ghost note for: " + commitSha);
    std::string note = git::Notes::show("refs/notes/ghost", commitSha);
    bool gitAiNote = false;
    if (note.empty()) {
        std::string repoRoot = git::Repo::getRoot();
        auto cfg = config::GhostConfigReader::load(repoRoot);
        if (cfg.gitai_fallback) {
            note = git::Notes::show("refs/notes/ai", commitSha);
            gitAiNote = !note.empty();
        }
    }
    if (note.empty()) {
        std::cout << output::Style::warning("  No Ghost note found for " + commitSha) << "\n";
        return kExitOk;
    }

    auto result = gitAiNote
        ? note::GitAiReader::parse(note)
        : note::NoteReader::parse(note);
    if (!result.success) {
        std::cout << output::Style::error("  Failed to parse note: " + result.error) << "\n";
        std::cout << "\n" << output::Style::dim(note) << "\n";
        return kExitOk;
    }

    using namespace ghost::output;
    std::cout << Style::header("Commit Attribution");
    std::cout << "  " << Style::label("sha") << " " << Style::violet(commitSha) << "\n\n";
    if (gitAiNote) {
        std::cout << "  " << Style::dim("source refs/notes/ai (git-ai fallback)") << "\n\n";
    }

    for (const auto& entry : result.entries) {
        std::cout << "  " << Style::blue(entry.file_path) << "\n";
        auto it = result.sessions.find(entry.session_id);
        if (it != result.sessions.end()) {
            const auto& sess = it->second;
            std::cout << "    " << Style::muted(entry.session_id)
                      << "  " << Style::progressBar(100, 100, 5)
                      << "  " << Style::glow(sess.agent) << Style::dim("/") << Style::glow(sess.model) << "\n";
        } else {
            std::cout << "    " << Style::muted(entry.session_id)
                      << "  " << Style::violet(entry.ranges.toString()) << "\n";
        }
    }
    std::cout << "\n";
    return kExitOk;
}

int blame(int argc, char* argv[], bool verbose) {
    if (argc < 3 || std::string(argv[2])[0] == '-') {
        CommandRegistry::printHelp("blame");
        return kExitError;
    }
    std::string filePath = argv[2];
    std::string repoRoot = git::Repo::getRoot();
    if (repoRoot.empty()) {
        std::cerr << output::Style::error("Not in a git repository") << "\n";
        return kExitNotInRepo;
    }
    std::string headSha = git::Repo::getHead();
    bool jsonOutput = hasFlag(argc, argv, "--json") || hasFlag(argc, argv, "-j");
    auto cfg = config::GhostConfigReader::load(repoRoot);
    logVerbose(verbose, "blame for: " + filePath + " @ " + headSha);

    auto blameData = git::Blame::getLineAuthorMap(filePath);
    if (blameData.empty()) {
        std::cout << output::Style::warning("No blame data for " + filePath) << "\n";
        return kExitOk;
    }

    std::map<std::string, note::NoteReader::Result> ghostNotes;
    {
        std::set<std::string> allShas;
        for (const auto& commitSha : blameData.lines) {
            allShas.insert(commitSha);
        }
        allShas.insert(headSha);
        std::vector<std::string> shaVec(allShas.begin(), allShas.end());
        auto batchNotes = git::Notes::showBatch("refs/notes/ghost", shaVec);
        for (const auto& [sha, raw] : batchNotes) {
            if (!raw.empty()) {
                ghostNotes[sha] = note::NoteReader::parse(raw);
            }
        }
        if (cfg.gitai_fallback) {
            auto gitAiNotes = git::Notes::showBatch("refs/notes/ai", shaVec);
            for (const auto& [sha, raw] : gitAiNotes) {
                if (!raw.empty() && ghostNotes.count(sha) == 0) {
                    ghostNotes[sha] = note::GitAiReader::parse(raw);
                }
            }
        }
    }

    auto attribution = audit::BlameOverlay::overlay(filePath, blameData, ghostNotes);

    if (jsonOutput) {
        std::cout << "{\n";
        std::cout << "  \"file\": \"" << util::Json::escape(filePath) << "\",\n";
        std::cout << "  \"total_lines\": " << attribution.total_lines << ",\n";
        std::cout << "  \"ai_lines\": " << attribution.ai_lines << ",\n";
        std::cout << "  \"lines\": [\n";
        for (size_t i = 0; i < attribution.lines.size(); ++i) {
            const auto& l = attribution.lines[i];
            std::cout << "    {\"line\": " << l.line_number
                      << ", \"commit\": \"" << util::Json::escape(l.commit_sha)
                      << "\", \"is_ai\": " << (l.is_ai ? "true" : "false");
            if (l.is_ai) {
                std::cout << ", \"agent\": \"" << util::Json::escape(l.agent)
                          << "\", \"model\": \"" << util::Json::escape(l.model) << "\"";
            }
            std::cout << "}";
            if (i + 1 < attribution.lines.size()) std::cout << ",";
            std::cout << "\n";
        }
        std::cout << "  ]\n";
        std::cout << "}\n";
        return kExitOk;
    }

    bool hasTerm = std::getenv("TERM") != nullptr && std::getenv("NO_COLOR") == nullptr;
    auto v = [&](const std::string& s) { return hasTerm ? "\033[38;5;141m" + s + "\033[0m" : s; };
    auto b = [&](const std::string& s) { return hasTerm ? "\033[38;5;75m" + s + "\033[0m" : s; };
    auto w = [&](const std::string& s) { return hasTerm ? "\033[38;5;231m" + s + "\033[0m" : s; };
    auto d = [&](const std::string& s) { return hasTerm ? "\033[2m\033[38;5;248m" + s + "\033[0m" : s; };
    for (const auto& l : attribution.lines) {
        std::string tag = l.is_ai ? v("AI  ") : d("human");
        std::cout << d(std::to_string(l.line_number)) << " "
                  << b(l.commit_sha.substr(0, 8)) << " "
                  << tag;
        if (l.is_ai) {
            std::cout << " " << d("|") << " " << w(l.agent) << " " << d("/") << " " << w(l.model);
        }
        std::cout << "\n";
    }
    int pct = attribution.total_lines > 0
        ? std::min((attribution.ai_lines * 100) / attribution.total_lines, 100) : 0;
    std::cout << "\n" << d(std::to_string(attribution.ai_lines) + "/" + std::to_string(attribution.total_lines))
              << " AI lines (" << v(std::to_string(pct) + "%") << ")\n";
    return kExitOk;
}

int stats(int argc, char* argv[], bool verbose) {
    std::string repoRoot = git::Repo::getRoot();
    if (repoRoot.empty()) {
        std::cerr << output::Style::error("Not in a git repository") << "\n";
        return kExitNotInRepo;
    }
    std::string range = "HEAD~1..HEAD";
    if (argc > 2 && std::string(argv[2])[0] != '-') {
        range = argv[2];
    }
    bool jsonOutput = hasFlag(argc, argv, "--json") || hasFlag(argc, argv, "-j");
    logVerbose(verbose, "stats range: " + range);

    auto report = audit::Auditor::run(repoRoot, range, -1, false);
    if (jsonOutput) {
        std::cout << "{\n";
        std::cout << "  \"total_commits\": " << report.summary.commits.size() << ",\n";
        std::cout << "  \"total_lines\": " << report.summary.total_lines << ",\n";
        std::cout << "  \"ai_lines\": " << report.summary.ai_lines << ",\n";
        std::cout << "  \"ai_percent\": " << (report.summary.total_lines > 0
            ? std::min((report.summary.ai_lines * 100.0) / report.summary.total_lines, 100.0) : 0.0) << ",\n";
        std::cout << "  \"commits\": [\n";
        for (size_t i = 0; i < report.summary.commits.size(); ++i) {
            const auto& c = report.summary.commits[i];
            double cpct = c.total_lines > 0 ? std::min((c.ai_lines * 100.0) / c.total_lines, 100.0) : 0.0;
            std::cout << "    {\"commit\": \"" << util::Json::escape(c.commit_sha)
                      << "\", \"ai_lines\": " << c.ai_lines
                      << ", \"total_lines\": " << c.total_lines
                      << ", \"ai_percent\": " << cpct << "}";
            if (i + 1 < report.summary.commits.size()) std::cout << ",";
            std::cout << "\n";
        }
        std::cout << "  ]\n";
        std::cout << "}\n";
        return kExitOk;
    }

    bool hasTerm = std::getenv("TERM") != nullptr && std::getenv("NO_COLOR") == nullptr;
    auto v = [&](const std::string& s) { return hasTerm ? "\033[38;5;141m" + s + "\033[0m" : s; };
    auto b = [&](const std::string& s) { return hasTerm ? "\033[38;5;75m" + s + "\033[0m" : s; };
    auto d = [&](const std::string& s) { return hasTerm ? "\033[2m\033[38;5;248m" + s + "\033[0m" : s; };
    for (const auto& c : report.summary.commits) {
        int cpct = c.total_lines > 0 ? std::min((c.ai_lines * 100) / c.total_lines, 100) : 0;
        std::cout << "  " << b(c.commit_sha.substr(0, 8)) << "  "
                  << v(std::to_string(cpct) + "%") << " "
                  << d("(" + std::to_string(c.ai_lines) + "/" + std::to_string(c.total_lines) + " lines)") << "\n";
    }
    if (report.summary.commits.size() > 1) {
        int apct = report.summary.total_lines > 0
            ? std::min((report.summary.ai_lines * 100) / report.summary.total_lines, 100) : 0;
        std::cout << "\n  " << d("total") << "  " << v(std::to_string(apct) + "%") << " "
                  << d("(" + std::to_string(report.summary.ai_lines) + "/" + std::to_string(report.summary.total_lines) + " lines)") << "\n";
    }
    return kExitOk;
}

}
}
