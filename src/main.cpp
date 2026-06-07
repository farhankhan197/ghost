#include <iostream>
#include <string>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <set>
#include <map>
#include <vector>
#include <cctype>
#include <filesystem>
#include <memory>
#include <cstdio>
#include "git/repo.hpp"
#include "git/notes.hpp"
#include "git/blame.hpp"
#include "git/diff.hpp"
#include "note/reader.hpp"
#include "note/gitai_reader.hpp"
#include "commit/post_commit.hpp"
#include "commit/note_index.hpp"
#include "checkpoint/working_log.hpp"
#include "checkpoint/session.hpp"
#include "hooks/installer.hpp"
#include "hooks/agent_hooks.hpp"
#include "hooks/agent_detector.hpp"
#include "audit/auditor.hpp"
#include "audit/blame_overlay.hpp"
#include "audit/aggregator.hpp"
#include "audit/policy.hpp"
#include "output/report.hpp"
#include "output/style.hpp"
#include "output/interactive.hpp"
#include "config/ghost_config.hpp"
#include "cli/commands.hpp"
#include "persist/db.hpp"
#include "rewrite/rewrite_log.hpp"
#include "rewrite/processor.hpp"
#include "rewrite/working_state.hpp"
#include <set>

// Verbose logging utility
static bool g_verbose = false;
static void logVerbose(const std::string& msg) {
    if (g_verbose) {
        std::cerr << ghost::output::Style::dim("[verbose] " + msg) << "\n";
    }
}

static bool hasFlag(int argc, char* argv[], const std::string& flag) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == flag) return true;
    }
    return false;
}

static std::string getArg(int argc, char* argv[], const std::string& flag) {
    for (int i = 0; i < argc - 1; ++i) {
        if (std::string(argv[i]) == flag && i + 1 < argc) {
            return argv[i + 1];
        }
    }
    return "";
}

static std::string normalizeRepoPath(const std::string& path, const std::string& repoRoot) {
    if (path.empty()) return "";
    std::string normalized = path;
    std::error_code ec;
    std::filesystem::path p(path);
    if (p.is_absolute() && !repoRoot.empty()) {
        auto rel = std::filesystem::relative(p, repoRoot, ec);
        if (!ec) normalized = rel.string();
    }
    for (char& c : normalized) {
        if (c == '\\') c = '/';
    }
    while (normalized.rfind("./", 0) == 0) {
        normalized = normalized.substr(2);
    }
    return normalized;
}

static std::string canonicalPathString(const std::string& path) {
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(std::filesystem::path(path), ec);
    return (ec ? std::filesystem::path(path) : canonical).string();
}

static bool samePath(const std::string& a, const std::string& b) {
#ifdef _WIN32
    std::string left = canonicalPathString(a);
    std::string right = canonicalPathString(b);
    std::transform(left.begin(), left.end(), left.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(right.begin(), right.end(), right.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return left == right;
#else
    return canonicalPathString(a) == canonicalPathString(b);
#endif
}

static std::string findRepoRootForPath(const std::string& path) {
    if (path.empty()) return "";
    std::error_code ec;
    std::filesystem::path p(path);
    if (!p.is_absolute()) {
        p = std::filesystem::absolute(p, ec);
        if (ec) return "";
    }
    std::filesystem::path dir = std::filesystem::is_directory(p, ec) ? p : p.parent_path();
    while (!dir.empty()) {
        if (std::filesystem::exists(dir / ".git", ec)) {
            return dir.string();
        }
        auto parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return "";
}

static std::string extractJsonStringValue(const std::string& json, const std::string& key, size_t startAt = 0) {
    std::string search = "\"" + key + "\":";
    size_t pos = json.find(search, startAt);
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
            result += c;
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

static std::vector<std::string> extractSessionFiles(const std::string& jsonData, const std::string& repoRoot) {
    std::vector<std::string> files;
    size_t pos = 0;
    while ((pos = jsonData.find("\"file_path\":", pos)) != std::string::npos) {
        std::string file = normalizeRepoPath(extractJsonStringValue(jsonData, "file_path", pos), repoRoot);
        if (!file.empty() && std::find(files.begin(), files.end(), file) == files.end()) {
            files.push_back(file);
        }
        pos += 12;
    }
    return files;
}

static bool sessionTouchesFile(const ghost::persist::Session& session, const std::string& filePath, const std::string& repoRoot) {
    std::string target = normalizeRepoPath(filePath, repoRoot);
    auto files = extractSessionFiles(session.json_data, repoRoot);
    return std::find(files.begin(), files.end(), target) != files.end();
}

static bool sessionBelongsToRepo(const ghost::persist::Session& session, const std::string& repoRoot) {
    auto files = extractSessionFiles(session.json_data, repoRoot);
    if (files.empty()) return true;
    for (const auto& file : files) {
        std::string owner = findRepoRootForPath((std::filesystem::path(repoRoot) / file).string());
        if (!owner.empty() && !samePath(owner, repoRoot)) {
            return false;
        }
    }
    return true;
}

// Exit codes (avoid standard macro conflicts)
static constexpr int GHOST_EXIT_OK = 0;
static constexpr int GHOST_EXIT_ERROR = 1;
static constexpr int GHOST_EXIT_BLOCKED = 2;
static constexpr int GHOST_EXIT_NOT_IN_REPO = 3;

static std::string execCommand(const std::string& cmd) {
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) return "";
    char buffer[1024];
    while (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
        result += buffer;
    }
    // Trim trailing whitespace
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r' || result.back() == ' ')) {
        result.pop_back();
    }
    return result;
}

static bool fileExists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

static void printSuggestion(const std::string& unknown) {
    auto suggestions = ghost::cli::CommandRegistry::getSuggestions(unknown);
    if (!suggestions.empty()) {
        std::cerr << "\n" << ghost::output::Style::dim("Did you mean?");
        for (const auto& s : suggestions) {
            std::cerr << "  " << ghost::output::Style::violet("ghost " + s);
        }
        std::cerr << "\n";
    }
    std::cerr << ghost::output::Style::dim("Run 'ghost help' for all available commands.\n");
}

static int handleInit(int argc, char* argv[]);

static int handleInstall(int argc, char* argv[]) {
    logVerbose("processing install command (deprecated, redirecting to init)");
    using namespace ghost::output;
    std::cout << Style::dim("'ghost install' is deprecated. Use 'ghost init --yes' instead.\n\n");
    
    bool global = hasFlag(argc, argv, "--global") || hasFlag(argc, argv, "-g");
    if (global) {
        return ghost::hooks::Installer::installGlobal();
    }
    
    // Build new argv with --yes flag appended
    std::vector<const char*> newArgv;
    newArgv.push_back(argv[0]);
    newArgv.push_back("init");
    newArgv.push_back("--yes");
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a != "--global" && a != "-g") {
            newArgv.push_back(argv[i]);
        }
    }
    int newArgc = static_cast<int>(newArgv.size());
    return handleInit(newArgc, const_cast<char**>(newArgv.data()));
}

static int handleUninstall(int argc, char* argv[]) {
    bool global = hasFlag(argc, argv, "--global") || hasFlag(argc, argv, "-g");
    if (global) {
        return ghost::hooks::Installer::uninstallGlobal();
    }
    std::string repoRoot = ghost::git::Repo::getRoot();
    if (repoRoot.empty()) {
        std::cerr << ghost::output::Style::error("Not in a git repository") << "\n";
        return GHOST_EXIT_NOT_IN_REPO;
    }
    return ghost::hooks::Installer::uninstallRepo(repoRoot);
}

static int handleShow(int argc, char* argv[]) {
    if (argc < 3 || std::string(argv[2])[0] == '-') {
        ghost::cli::CommandRegistry::printHelp("show");
        return GHOST_EXIT_ERROR;
    }
    std::string commit_sha = argv[2];
    logVerbose("showing ghost note for: " + commit_sha);
    std::string note = ghost::git::Notes::show("refs/notes/ghost", commit_sha);
    bool gitAiNote = false;
    if (note.empty()) {
        std::string repoRoot = ghost::git::Repo::getRoot();
        auto cfg = ghost::config::GhostConfigReader::load(repoRoot);
        if (cfg.gitai_fallback) {
            note = ghost::git::Notes::show("refs/notes/ai", commit_sha);
            gitAiNote = !note.empty();
        }
    }
    if (note.empty()) {
        std::cout << ghost::output::Style::warning("  No ghost note found for " + commit_sha) << "\n";
    } else {
        auto result = gitAiNote
            ? ghost::note::GitAiReader::parse(note)
            : ghost::note::NoteReader::parse(note);
        if (!result.success) {
            std::cout << ghost::output::Style::error("  Failed to parse note: " + result.error) << "\n";
            std::cout << "\n" << ghost::output::Style::dim(note) << "\n";
        } else {
            using namespace ghost::output;
            std::cout << Style::header("Commit Attribution");
            std::cout << "  " << Style::label("sha") << " " << Style::violet(commit_sha) << "\n\n";
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
        }
    }
    return GHOST_EXIT_OK;
}

static int handleAudit(int argc, char* argv[]) {
    std::string repoRoot = ghost::git::Repo::getRoot();
    if (repoRoot.empty()) {
        std::cerr << ghost::output::Style::error("Not in a git repository") << "\n";
        return GHOST_EXIT_NOT_IN_REPO;
    }
    std::string range = getArg(argc, argv, "--range");
    bool allMode = hasFlag(argc, argv, "--all") || hasFlag(argc, argv, "-a");
    std::string thresholdStr = getArg(argc, argv, "--threshold");
    int threshold = -1;
    if (!thresholdStr.empty()) {
        try { threshold = std::stoi(thresholdStr); } catch (...) {}
    }
    std::string configRef = getArg(argc, argv, "--config-ref");
    bool jsonOutput = hasFlag(argc, argv, "--json") || hasFlag(argc, argv, "-j");

    logVerbose("audit mode: " + std::string(allMode ? "all" : (range.empty() ? "head" : "range")));
    if (!configRef.empty()) logVerbose("config ref: " + configRef);
    
    if (allMode || !range.empty()) {
        ghost::output::AnimatedSpinner spinner("scanning commits...");
        ghost::audit::AuditReport report;
        if (!range.empty()) {
            logVerbose("range: " + range);
            report = ghost::audit::Auditor::run(repoRoot, range, threshold, jsonOutput, configRef);
        } else {
            std::vector<std::string> commitShas = ghost::audit::Auditor::getCommitsWithGhostNotes();
            logVerbose("found " + std::to_string(commitShas.size()) + " commits with ghost notes");
            report = ghost::audit::Auditor::runFromList(repoRoot, commitShas, threshold, jsonOutput, configRef);
        }
        spinner.stop();
        if (jsonOutput) {
            std::cout << ghost::output::Report::formatJSON(report.summary, report.policy);
        } else {
            std::cout << ghost::output::Report::formatCLI(report.summary, report.policy, true);
        }
        return report.policy.blocked ? GHOST_EXIT_BLOCKED : GHOST_EXIT_OK;
    } else if (argc > 2 && std::string(argv[2])[0] != '-') {
        std::string target = argv[2];
        logVerbose("single commit audit: " + target);
        ghost::output::AnimatedSpinner spinner("scanning codebase...");
        auto cbReport = ghost::audit::Auditor::runCodebaseBlame(repoRoot, target, threshold, jsonOutput, configRef);
        spinner.stop();
        if (jsonOutput) {
            std::cout << ghost::output::Report::formatCodebaseJSON(cbReport.summary, cbReport.policy);
        } else {
            ghost::output::Report::streamCodebaseCLI(cbReport.summary, cbReport.policy);
        }
        return cbReport.policy.blocked ? GHOST_EXIT_BLOCKED : GHOST_EXIT_OK;
    } else {
        ghost::output::AnimatedSpinner spinner("scanning codebase...");
        auto cbReport = ghost::audit::Auditor::runCodebaseBlame(repoRoot, "HEAD", threshold, jsonOutput, configRef);
        spinner.stop();
        if (jsonOutput) {
            std::cout << ghost::output::Report::formatCodebaseJSON(cbReport.summary, cbReport.policy);
        } else {
            ghost::output::Report::streamCodebaseCLI(cbReport.summary, cbReport.policy);
        }
        return cbReport.policy.blocked ? GHOST_EXIT_BLOCKED : GHOST_EXIT_OK;
    }
}

static int handleBlame(int argc, char* argv[]) {
    if (argc < 3 || std::string(argv[2])[0] == '-') {
        ghost::cli::CommandRegistry::printHelp("blame");
        return GHOST_EXIT_ERROR;
    }
    std::string filePath = argv[2];
    std::string repoRoot = ghost::git::Repo::getRoot();
    if (repoRoot.empty()) {
        std::cerr << ghost::output::Style::error("Not in a git repository") << "\n";
        return GHOST_EXIT_NOT_IN_REPO;
    }
    std::string headSha = ghost::git::Repo::getHead();
    bool jsonOutput = hasFlag(argc, argv, "--json") || hasFlag(argc, argv, "-j");
    auto cfg = ghost::config::GhostConfigReader::load(repoRoot);
    logVerbose("blame for: " + filePath + " @ " + headSha);

    auto blame = ghost::git::Blame::getLineAuthorMap(filePath);
    if (blame.empty()) {
        std::cout << ghost::output::Style::warning("No blame data for " + filePath) << "\n";
        return GHOST_EXIT_OK;
    }

    std::map<std::string, ghost::note::NoteReader::Result> ghostNotes;
    {
        std::set<std::string> allShas;
        for (const auto& commitSha : blame.lines) {
            allShas.insert(commitSha);
        }
        allShas.insert(headSha);
        std::vector<std::string> shaVec(allShas.begin(), allShas.end());
        auto batchNotes = ghost::git::Notes::showBatch("refs/notes/ghost", shaVec);
        for (const auto& [sha, raw] : batchNotes) {
            if (!raw.empty()) {
                ghostNotes[sha] = ghost::note::NoteReader::parse(raw);
            }
        }
        if (cfg.gitai_fallback) {
            auto gitAiNotes = ghost::git::Notes::showBatch("refs/notes/ai", shaVec);
            for (const auto& [sha, raw] : gitAiNotes) {
                if (!raw.empty() && ghostNotes.count(sha) == 0) {
                    ghostNotes[sha] = ghost::note::GitAiReader::parse(raw);
                }
            }
        }
    }

    auto attribution = ghost::audit::BlameOverlay::overlay(filePath, blame, ghostNotes);

    if (jsonOutput) {
        std::cout << "{\n";
        std::cout << "  \"file\": \"" << filePath << "\",\n";
        std::cout << "  \"total_lines\": " << attribution.total_lines << ",\n";
        std::cout << "  \"ai_lines\": " << attribution.ai_lines << ",\n";
        std::cout << "  \"lines\": [\n";
        for (size_t i = 0; i < attribution.lines.size(); ++i) {
            const auto& l = attribution.lines[i];
            std::cout << "    {\"line\": " << l.line_number
                      << ", \"commit\": \"" << l.commit_sha
                      << "\", \"is_ai\": " << (l.is_ai ? "true" : "false");
            if (l.is_ai) {
                std::cout << ", \"agent\": \"" << l.agent
                          << "\", \"model\": \"" << l.model << "\"";
            }
            std::cout << "}";
            if (i + 1 < attribution.lines.size()) std::cout << ",";
            std::cout << "\n";
        }
        std::cout << "  ]\n";
        std::cout << "}\n";
    } else {
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
    }
    return GHOST_EXIT_OK;
}

static int handleStats(int argc, char* argv[]) {
    std::string repoRoot = ghost::git::Repo::getRoot();
    if (repoRoot.empty()) {
        std::cerr << ghost::output::Style::error("Not in a git repository") << "\n";
        return GHOST_EXIT_NOT_IN_REPO;
    }
    std::string range = "HEAD~1..HEAD";
    if (argc > 2 && std::string(argv[2])[0] != '-') {
        range = argv[2];
    }
    bool jsonOutput = hasFlag(argc, argv, "--json") || hasFlag(argc, argv, "-j");
    logVerbose("stats range: " + range);

    auto report = ghost::audit::Auditor::run(repoRoot, range, -1, false);
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
            std::cout << "    {\"commit\": \"" << c.commit_sha
                      << "\", \"ai_lines\": " << c.ai_lines
                      << ", \"total_lines\": " << c.total_lines
                      << ", \"ai_percent\": " << cpct << "}";
            if (i + 1 < report.summary.commits.size()) std::cout << ",";
            std::cout << "\n";
        }
        std::cout << "  ]\n";
        std::cout << "}\n";
    } else {
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
    }
    return GHOST_EXIT_OK;
}

static int handleConfig(int argc, char* argv[]) {
    std::string repoRoot = ghost::git::Repo::getRoot();
    if (repoRoot.empty()) {
        std::cerr << ghost::output::Style::error("Not in a git repository") << "\n";
        return GHOST_EXIT_NOT_IN_REPO;
    }
    if (argc > 2 && std::string(argv[2]) == "set" && argc >= 5) {
        logVerbose("config set: " + std::string(argv[3]) + " = " + argv[4]);
        if (ghost::config::GhostConfigReader::save(repoRoot, argv[3], argv[4])) {
            std::cout << ghost::output::Style::success("Set " + std::string(argv[3]) + " = " + argv[4]) << "\n";
        } else {
            std::cerr << ghost::output::Style::error("Failed to write ghost.yml") << "\n";
            return GHOST_EXIT_ERROR;
        }
    } else {
        auto cfg = ghost::config::GhostConfigReader::load(repoRoot);
        bool hasTerm = std::getenv("TERM") != nullptr && std::getenv("NO_COLOR") == nullptr;
        auto v = [&](const std::string& s) { return hasTerm ? "\033[38;5;141m" + s + "\033[0m" : s; };
        auto b = [&](const std::string& s) { return hasTerm ? "\033[38;5;75m" + s + "\033[0m" : s; };
        auto w = [&](const std::string& s) { return hasTerm ? "\033[38;5;231m" + s + "\033[0m" : s; };
        auto d = [&](const std::string& s) { return hasTerm ? "\033[2m\033[38;5;248m" + s + "\033[0m" : s; };
        auto g = [&](const std::string& s) { return hasTerm ? "\033[32m" + s + "\033[0m" : s; };
        std::cout << b("version") << "    " << w(std::to_string(cfg.version)) << "\n";
        std::cout << b("required") << "   " << (cfg.required ? g("true") : d("false")) << "\n";
        std::cout << b("threshold") << "  " << w(std::to_string(cfg.threshold)) << "\n";
        std::cout << b("on_exceed") << "  " << w(cfg.on_exceed) << "\n";
        std::cout << b("pr_comment") << " " << (cfg.pr_comment ? g("true") : d("false")) << "\n";
        std::cout << b("untagged") << "   " << w(cfg.untagged_policy) << "\n";
        std::cout << b("unverified") << " " << w(cfg.unverified_policy) << "\n";
        std::cout << b("gitai_fb") << "   " << (cfg.gitai_fallback ? g("true") : d("false")) << "\n";
        if (!cfg.ignore.empty()) {
            std::cout << b("ignore") << "     " << w(cfg.ignore[0]);
            for (size_t i = 1; i < cfg.ignore.size(); ++i) {
                std::cout << d(", ") << w(cfg.ignore[i]);
            }
            std::cout << "\n";
        }
    }
    return GHOST_EXIT_OK;
}

static int handleBanish(int argc, char* argv[]) {
    std::string repoRoot = ghost::git::Repo::getRoot();
    if (repoRoot.empty()) {
        std::cerr << ghost::output::Style::error("Not in a git repository") << "\n";
        return GHOST_EXIT_NOT_IN_REPO;
    }

    auto cfg = ghost::config::GhostConfigReader::load(repoRoot);

    // Check owner authorization
    std::string currentUser = ghost::git::Repo::getUserEmail();
    if (cfg.owner.empty()) {
        std::cerr << ghost::output::Style::error("No owner configured for this repo.\n")
                  << ghost::output::Style::dim("  Set the owner with: ghost config set owner <email>\n");
        return GHOST_EXIT_ERROR;
    }
    if (currentUser.empty() || currentUser != cfg.owner) {
        std::cerr << ghost::output::Style::error("Only the repo owner (" + cfg.owner + ") can banish files.\n")
                  << ghost::output::Style::dim("  Current user: " + (currentUser.empty() ? "unknown" : currentUser) + "\n");
        return GHOST_EXIT_ERROR;
    }

    using namespace ghost::output;

    // --list: show currently banished paths
    if (hasFlag(argc, argv, "--list")) {
        if (cfg.ignore.empty()) {
            std::cout << Style::dim("No files are banished.\n");
        } else {
            std::cout << Style::header("Banished Paths");
            for (const auto& p : cfg.ignore) {
                std::cout << "  " << Style::violet(p) << "\n";
            }
        }
        return GHOST_EXIT_OK;
    }

    // --clear: remove patterns from the ignore list
    if (hasFlag(argc, argv, "--clear")) {
        // Collect all positional args after --clear as files to un-banish
        std::vector<std::string> toClear;
        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--clear") continue;
            if (arg == "--verbose" || arg == "-v") continue;
            if (arg.size() > 1 && arg[0] == '-') continue;
            toClear.push_back(arg);
        }

        std::vector<std::string> newIgnore;
        if (toClear.empty()) {
            // Clear all
            logVerbose("clearing all banished paths");
        } else {
            // Remove specific paths
            logVerbose("clearing " + std::to_string(toClear.size()) + " path(s) from banish list");
            for (const auto& p : cfg.ignore) {
                bool keep = true;
                for (const auto& c : toClear) {
                    if (p == c) { keep = false; break; }
                }
                if (keep) newIgnore.push_back(p);
            }
        }

        if (ghost::config::GhostConfigReader::saveIgnore(repoRoot, newIgnore)) {
            std::cout << Style::success("Updated banished paths.") << "\n";
        } else {
            std::cerr << Style::error("Failed to write ghost.yml") << "\n";
            return GHOST_EXIT_ERROR;
        }
        return GHOST_EXIT_OK;
    }

    // Collect positional args as file paths to banish
    std::vector<std::string> newPaths;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.size() > 1 && arg[0] == '-') continue;
        newPaths.push_back(arg);
    }

    if (newPaths.empty()) {
        std::cerr << Style::error("No files specified.\n")
                  << Style::dim("  Usage: ghost banish <path> [<path> ...]\n")
                  << Style::dim("         ghost banish --list\n")
                  << Style::dim("         ghost banish --clear [<path> ...]\n");
        return GHOST_EXIT_ERROR;
    }

    // Merge existing ignore list with new paths (dedup)
    std::set<std::string> merged;
    for (const auto& p : cfg.ignore) merged.insert(p);
    for (const auto& p : newPaths) merged.insert(p);
    std::vector<std::string> ignoreVec(merged.begin(), merged.end());

    if (!ghost::config::GhostConfigReader::saveIgnore(repoRoot, ignoreVec)) {
        std::cerr << Style::error("Failed to write ghost.yml") << "\n";
        return GHOST_EXIT_ERROR;
    }

    std::cout << Style::success("Banished " + std::to_string(newPaths.size()) + " file(s) from AI tracking.") << "\n";
    for (const auto& p : newPaths) {
        std::cout << "  " << Style::violet(p) << "\n";
    }
    logVerbose("total banished patterns: " + std::to_string(ignoreVec.size()));
    return GHOST_EXIT_OK;
}

static int handleInstallHooks(int argc, char* argv[]) {
    std::string repoRoot = ghost::git::Repo::getRoot();
    if (repoRoot.empty()) {
        std::cerr << ghost::output::Style::error("Not in a git repository") << "\n";
        return GHOST_EXIT_NOT_IN_REPO;
    }
    bool global = !hasFlag(argc, argv, "--repo");
    std::string specificAgent;
    for (int i = 2; i < argc - 1; ++i) {
        if (std::string(argv[i]) == "--agent" && i + 1 < argc) {
            specificAgent = argv[i + 1];
            break;
        }
    }

    logVerbose("install hooks, global=" + std::to_string(global) + ", agent=" + specificAgent);
    if (!specificAgent.empty()) {
        if (!ghost::hooks::AgentHooks::installForAgent(repoRoot, specificAgent, global)) return GHOST_EXIT_ERROR;
    } else {
        if (!ghost::hooks::AgentHooks::installAll(repoRoot, global)) return GHOST_EXIT_ERROR;
    }
    return GHOST_EXIT_OK;
}

static int handleUninstallHooks(int argc, char* argv[]) {
    std::string repoRoot = ghost::git::Repo::getRoot();
    if (repoRoot.empty()) {
        std::cerr << ghost::output::Style::error("Not in a git repository") << "\n";
        return GHOST_EXIT_NOT_IN_REPO;
    }
    bool global = !hasFlag(argc, argv, "--repo");
    std::string specificAgent;
    for (int i = 2; i < argc - 1; ++i) {
        if (std::string(argv[i]) == "--agent" && i + 1 < argc) {
            specificAgent = argv[i + 1];
            break;
        }
    }

    logVerbose("uninstall hooks, global=" + std::to_string(global) + ", agent=" + specificAgent);
    if (!specificAgent.empty()) {
        ghost::hooks::AgentHooks::uninstallForAgent(repoRoot, specificAgent, global);
    } else {
        ghost::hooks::AgentHooks::uninstallAll(repoRoot, global);
    }
    return GHOST_EXIT_OK;
}

static int handleInit(int argc, char* argv[]) {
    bool global = hasFlag(argc, argv, "--global") || hasFlag(argc, argv, "-g");
    if (global) {
        return ghost::hooks::Installer::installGlobal();
    }

    std::string repoRoot = ghost::git::Repo::getRoot();
    if (repoRoot.empty()) {
        std::cerr << ghost::output::Style::error("Not in a git repository") << "\n";
        return GHOST_EXIT_NOT_IN_REPO;
    }

    bool yesMode = hasFlag(argc, argv, "--yes") || hasFlag(argc, argv, "-y");
    bool interactive = hasFlag(argc, argv, "--interactive") || hasFlag(argc, argv, "-i");
    bool dryRun = hasFlag(argc, argv, "--dry-run") || hasFlag(argc, argv, "-n");

    logVerbose("init repo=" + repoRoot + " yes=" + std::to_string(yesMode) +
               " interactive=" + std::to_string(interactive) + " dryRun=" + std::to_string(dryRun));

    using namespace ghost::output;
    using namespace ghost::output::interactive;

    // Default config values
    int threshold = 80;
    bool required = false;
    std::string onExceed = "block";
    bool prComment = true;
    std::string untaggedPolicy = "human";
    std::string unverifiedPolicy = "warn";
    bool gitaiFallback = true;
    std::vector<std::string> ignorePatterns;
    std::vector<std::string> selectedAgents;

    // Smart defaults: detect common build dirs
    if (fileExists(repoRoot + "/node_modules")) {
        ignorePatterns.push_back("node_modules/");
    }
    if (fileExists(repoRoot + "/build")) {
        ignorePatterns.push_back("build/");
    }
    if (fileExists(repoRoot + "/dist")) {
        ignorePatterns.push_back("dist/");
    }
    if (fileExists(repoRoot + "/target")) {
        ignorePatterns.push_back("target/");  // Rust
    }
    if (fileExists(repoRoot + "/.next")) {
        ignorePatterns.push_back(".next/");
    }
    ignorePatterns.push_back(".git/");

    // Interactive wizard
    if (interactive && isInteractive()) {
        std::cout << Style::header("Ghost Setup Wizard") << "\n";
        std::cout << Style::dim("  Configure ghost for this repository.\n\n");

        // Step 1: Threshold
        std::vector<std::string> thresholdOpts = {"80% (default)", "50% (strict)", "30% (very strict)", "Custom"};
        int threshChoice = selectMenu("AI attribution threshold", thresholdOpts, 0);
        if (threshChoice < 0) return GHOST_EXIT_ERROR;
        if (threshChoice == 3) {
            std::string custom = inputPrompt("Enter threshold percentage (0-100)", "80");
            try { threshold = std::stoi(custom); } catch (...) { threshold = 80; }
        } else if (threshChoice == 1) {
            threshold = 50;
        } else if (threshChoice == 2) {
            threshold = 30;
        }

        // Step 2: Required
        std::vector<std::string> reqOpts = {"No (permissive)", "Yes (enforce on push)"};
        int reqChoice = selectMenu("Require ghost attribution?", reqOpts, 0);
        if (reqChoice < 0) return GHOST_EXIT_ERROR;
        required = (reqChoice == 1);

        // Step 3: On exceed
        std::vector<std::string> exceedOpts = {"block", "warn", "allow"};
        int exceedChoice = selectMenu("Policy when AI% exceeds threshold", exceedOpts, 0);
        if (exceedChoice < 0) return GHOST_EXIT_ERROR;
        onExceed = exceedOpts[exceedChoice];

        // Step 4: Agents
        auto detected = ghost::hooks::AgentDetector::detectInstalled();
        if (!detected.empty()) {
            std::cout << "\n" << Style::bold(Style::blue("Detected Agents")) << "\n";
            for (const auto& a : detected) {
                std::cout << "  " << Style::violet(a) << "\n";
            }
            std::cout << "\n";

            bool installAll = confirmPrompt("Install hooks for all detected agents?", true);
            if (!installAll) {
                for (const auto& a : detected) {
                    if (confirmPrompt("Install hook for " + a + "?", true)) {
                        selectedAgents.push_back(a);
                    }
                }
            } else {
                for (const auto& a : detected) {
                    selectedAgents.push_back(a);
                }
            }
        }

        // Step 5: Ignore patterns
        std::cout << "\n" << Style::bold(Style::blue("Ignore Patterns")) << "\n";
        std::cout << "  Pre-filled based on repo contents:\n";
        for (const auto& p : ignorePatterns) {
            std::cout << "    " << Style::dim("- " + p) << "\n";
        }
        if (confirmPrompt("Add custom ignore patterns?", false)) {
            std::string custom = inputPrompt("Enter pattern (e.g., '*.min.js', leave empty to finish)");
            while (!custom.empty()) {
                ignorePatterns.push_back(custom);
                custom = inputPrompt("Another pattern (leave empty to finish)");
            }
        }
    }

    // Dry run preview
    if (dryRun) {
        std::cout << Style::header("Dry Run — ghost init");
        std::cout << "Would configure:\n";
        std::cout << "  - ghost.yml (threshold=" << threshold << ", required=" << (required ? "true" : "false") << ")\n";
        std::cout << "  - post-commit hook\n";
        std::cout << "  - pre-push hook\n";
        std::cout << "  - git notes push refs\n";
        if (!selectedAgents.empty()) {
            std::cout << "  - agent hooks for: ";
            for (size_t i = 0; i < selectedAgents.size(); ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << selectedAgents[i];
            }
            std::cout << "\n";
        }
        if (yesMode) {
            std::cout << "  - binaries to ~/.ghost/bin (if not in PATH)\n";
        }
        std::cout << "\n";
        return GHOST_EXIT_OK;
    }

    // Write ghost.yml
    std::string ymlPath = repoRoot + "/ghost.yml";
    {
        std::ofstream yml(ymlPath);
        if (!yml) {
            std::cerr << Style::error("Failed to write ghost.yml") << "\n";
            return GHOST_EXIT_ERROR;
        }
        yml << "# Ghost configuration\n";
        yml << "# See: https://github.com/farhankhan197/ghost#configuration\n";
        yml << "\n";
        yml << "version: 1\n";
        yml << "threshold: " << threshold << "\n";
        yml << "required: " << (required ? "true" : "false") << "\n";
        yml << "on_exceed: " << onExceed << "\n";
        yml << "pr_comment: " << (prComment ? "true" : "false") << "\n";
        yml << "untagged: " << untaggedPolicy << "\n";
        yml << "unverified: " << unverifiedPolicy << "\n";
        yml << "gitai_fb: " << (gitaiFallback ? "true" : "false") << "\n";
        std::string ownerEmail = ghost::git::Repo::getUserEmail();
        if (!ownerEmail.empty()) {
            yml << "owner: " << ownerEmail << "\n";
        }
        if (!ignorePatterns.empty()) {
            yml << "ignore:\n";
            for (const auto& p : ignorePatterns) {
                yml << "  - " << p << "\n";
            }
        }
    }
    std::cout << "  " << Style::success("Created ghost.yml") << "\n";
    logVerbose("wrote ghost.yml to " + ymlPath);

    // Install hooks (but not binaries - init is hooks-only)
    int hooksResult = ghost::hooks::Installer::installRepo(repoRoot);
    if (hooksResult != GHOST_EXIT_OK) {
        std::cerr << Style::warning("Warning: some hooks may not have installed correctly") << "\n";
    }

    // Install agent hooks
    if (!selectedAgents.empty()) {
        for (const auto& agent : selectedAgents) {
            if (ghost::hooks::AgentHooks::installForAgent(repoRoot, agent, false)) {
                std::cout << "  " << Style::success("Installed hook for " + agent) << "\n";
            } else {
                std::cerr << Style::warning("  Could not install hook for " + agent) << "\n";
            }
        }
    }

    // Optionally install binaries
    if (yesMode) {
        std::string ghostPath = execCommand("which ghost 2>/dev/null || where ghost 2>nul");
        if (ghostPath.empty() || ghostPath.find("not found") != std::string::npos) {
            std::cout << "  " << Style::dim("ghost not found in PATH, installing binaries...") << "\n";
            int binResult = ghost::hooks::Installer::installBin();
            if (binResult == GHOST_EXIT_OK) {
                std::cout << "  " << Style::success("Installed binaries to ~/.ghost/bin") << "\n";
                std::cout << "  " << Style::warning("Add ~/.ghost/bin to your PATH to use ghost from anywhere") << "\n";
            } else {
                std::cerr << Style::warning("  Failed to install binaries. Run 'ghost init --yes' later.") << "\n";
            }
        }
    }

    std::cout << "\n" << Style::success("Done. Ghost is initialized in this repo.") << "\n";
    if (!yesMode) {
        std::cout << Style::dim("  Run 'ghost init --yes' to also install binaries.\n");
    }
    std::cout << "\n";
    return GHOST_EXIT_OK;
}

static int handleDoctor(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    using namespace ghost::output;

    std::cout << Style::header("Ghost Doctor");

    bool allOk = true;
    bool autoFix = hasFlag(argc, argv, "--fix") || hasFlag(argc, argv, "-f");

    // Check 1: Git repository
    std::string repoRoot = ghost::git::Repo::getRoot();
    if (repoRoot.empty()) {
        std::cout << "  " << Style::error("✗ Not in a git repository") << "\n";
        return GHOST_EXIT_ERROR;
    }
    std::cout << "  " << Style::success("✓ Git repository") << " " << Style::dim(repoRoot) << "\n";

    // Check 2: ghost binary in PATH
    {
        std::string ghostPath = execCommand("which ghost 2>/dev/null || where ghost 2>nul");
        if (ghostPath.empty() || ghostPath.find("not found") != std::string::npos) {
            std::cout << "  " << Style::warning("⚠ ghost not in PATH") << "\n";
            std::cout << "    " << Style::dim("Run 'ghost init --yes' or add ~/.ghost/bin to PATH") << "\n";
            allOk = false;
        } else {
            std::cout << "  " << Style::success("✓ ghost in PATH") << " " << Style::dim(ghostPath) << "\n";
        }
    }

    // Check 3: ghost.yml exists
    std::string ymlPath = repoRoot + "/ghost.yml";
    bool ymlExists = fileExists(ymlPath);
    if (!ymlExists) {
        std::cout << "  " << Style::warning("⚠ ghost.yml not found") << "\n";
        if (autoFix) {
            ghost::config::GhostConfigReader::save(repoRoot, "threshold", "80");
            std::cout << "    " << Style::success("Fixed: created ghost.yml with defaults") << "\n";
            ymlExists = true;
        } else {
            std::cout << "    " << Style::dim("Run 'ghost init' to create one") << "\n";
            allOk = false;
        }
    } else {
        auto cfg = ghost::config::GhostConfigReader::load(repoRoot);
        std::cout << "  " << Style::success("✓ ghost.yml") << " "
                  << Style::dim("threshold=" + std::to_string(cfg.threshold) +
                                  ", required=" + (cfg.required ? "true" : "false")) << "\n";
    }

    // Check 4: Hooks exist
    std::string postCommitHook = repoRoot + "/.git/hooks/post-commit";
    std::string prePushHook = repoRoot + "/.git/hooks/pre-push";
    bool postCommitExists = fileExists(postCommitHook);
    bool prePushExists = fileExists(prePushHook);

    if (!postCommitExists) {
        std::cout << "  " << Style::warning("⚠ post-commit hook missing") << "\n";
        if (autoFix) {
            ghost::hooks::Installer::installRepo(repoRoot);
            std::cout << "    " << Style::success("Fixed: installed hooks") << "\n";
        } else {
            allOk = false;
        }
    } else {
        std::cout << "  " << Style::success("✓ post-commit hook") << "\n";
    }

    if (!prePushExists) {
        std::cout << "  " << Style::warning("⚠ pre-push hook missing") << "\n";
        if (autoFix) {
            // Installer::installRepo installs both
            if (!postCommitExists) ghost::hooks::Installer::installRepo(repoRoot);
        } else {
            allOk = false;
        }
    } else {
        std::cout << "  " << Style::success("✓ pre-push hook") << "\n";
    }

    // Check 4b: History rewriting hooks
    {
        std::string hooks[] = {"post-rewrite", "post-merge", "post-checkout", "pre-merge-commit"};
        bool anyMissing = false;
        for (const auto& h : hooks) {
            std::string hookPath = repoRoot + "/.git/hooks/" + h;
            if (!fileExists(hookPath)) {
                std::cout << "  " << Style::warning("⚠ " + h + " hook missing") << "\n";
                anyMissing = true;
            } else {
                std::cout << "  " << Style::success("✓ " + h + " hook") << "\n";
            }
        }
        if (anyMissing && autoFix) {
            ghost::hooks::Installer::installRepo(repoRoot);
            std::cout << "    " << Style::success("Fixed: installed hooks") << "\n";
        }
        if (anyMissing && !autoFix) {
            allOk = false;
        }
    }

    // Check 5: Git notes refs configured
    {
        std::string remotePush = execCommand("git config --get remote.origin.push 2>/dev/null || echo ''");
        bool hasNotesRef = (remotePush.find("refs/notes/ghost") != std::string::npos);
        if (!hasNotesRef) {
            std::cout << "  " << Style::warning("⚠ git notes push not configured") << "\n";
            if (autoFix) {
                execCommand("git config --add remote.origin.push \"+refs/notes/ghost:refs/notes/ghost\"");
                execCommand("git config --add remote.origin.push \"+refs/notes/ghost-verified:refs/notes/ghost-verified\"");
                std::cout << "    " << Style::success("Fixed: configured notes push") << "\n";
            } else {
                std::cout << "    " << Style::dim("Run 'git config --add remote.origin.push +refs/notes/ghost:refs/notes/ghost'") << "\n";
                allOk = false;
            }
        } else {
            std::cout << "  " << Style::success("✓ git notes push configured") << "\n";
        }
    }

    // Check 6: Agent plugins
    auto detected = ghost::hooks::AgentDetector::detectInstalled();
    if (detected.empty()) {
        std::cout << "  " << Style::dim("  No AI agents detected") << "\n";
    } else {
        for (const auto& a : detected) {
            std::string agentDir = ghost::hooks::AgentDetector::getRepoConfigDir(a, repoRoot);
            bool hasHook = !agentDir.empty() && fileExists(agentDir);
            if (hasHook) {
                std::cout << "  " << Style::success("✓ " + a + " hook") << "\n";
            } else {
                std::cout << "  " << Style::warning("⚠ " + a + " detected but hook not installed") << "\n";
                if (autoFix) {
                    if (ghost::hooks::AgentHooks::installForAgent(repoRoot, a, false)) {
                        std::cout << "    " << Style::success("Fixed: installed " + a + " hook") << "\n";
                    }
                } else {
                    allOk = false;
                }
            }
        }
    }

    std::cout << "\n";
    if (allOk) {
        std::cout << Style::success("  All checks passed!") << "\n\n";
    } else {
        std::cout << Style::warning("  Some issues found. Run 'ghost doctor --fix' to auto-fix.") << "\n\n";
    }
    return allOk ? GHOST_EXIT_OK : GHOST_EXIT_ERROR;
}

static int handleStatus(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    using namespace ghost::output;

    std::string repoRoot = ghost::git::Repo::getRoot();
    std::string repoArg = getArg(argc, argv, "--repo");
    if (!repoArg.empty()) repoRoot = repoArg;
    if (repoRoot.empty() || !fileExists(repoRoot + "/.git")) {
        std::cerr << Style::error("Not in a git repository") << "\n";
        return GHOST_EXIT_NOT_IN_REPO;
    }
    std::error_code cwdEc;
    std::filesystem::current_path(repoRoot, cwdEc);

    std::cout << Style::header("Ghost Status");
    std::cout << "  Repo: " << repoRoot << "\n";
    std::cout << "  " << Style::dim("Overview of setup, working tree, uncommitted agent sessions, and HEAD notes.") << "\n\n";

    // Config
    auto cfg = ghost::config::GhostConfigReader::load(repoRoot);
    std::cout << Style::bold(Style::blue("  Policy Configuration")) << "\n";
    std::cout << "    threshold:   " << Style::glow(std::to_string(cfg.threshold) + "%") << "\n";
    std::cout << "    required:    " << (cfg.required ? Style::success("true") : Style::dim("false")) << "\n";
    std::cout << "    on_exceed:   " << Style::glow(cfg.on_exceed) << "\n";
    if (!cfg.ignore.empty()) {
        std::cout << "    ignore:      " << Style::dim(cfg.ignore[0]);
        for (size_t i = 1; i < cfg.ignore.size(); ++i) {
            std::cout << Style::dim(", " + cfg.ignore[i]);
        }
        std::cout << "\n";
    }
    std::cout << "\n";

    // Hooks
    std::cout << Style::bold(Style::blue("  Git Hooks")) << "\n";
    bool postCommit = fileExists(repoRoot + "/.git/hooks/post-commit");
    bool prePush = fileExists(repoRoot + "/.git/hooks/pre-push");
    bool postRewrite = fileExists(repoRoot + "/.git/hooks/post-rewrite");
    bool postMerge = fileExists(repoRoot + "/.git/hooks/post-merge");
    bool postCheckout = fileExists(repoRoot + "/.git/hooks/post-checkout");
    bool preMergeCommit = fileExists(repoRoot + "/.git/hooks/pre-merge-commit");
    std::cout << "    post-commit:       " << (postCommit ? Style::success("installed") : Style::warning("missing")) << "\n";
    std::cout << "    pre-push:          " << (prePush ? Style::success("installed") : Style::warning("missing")) << "\n";
    std::cout << "    post-rewrite:      " << (postRewrite ? Style::success("installed") : Style::warning("missing")) << "\n";
    std::cout << "    post-merge:        " << (postMerge ? Style::success("installed") : Style::warning("missing")) << "\n";
    std::cout << "    post-checkout:     " << (postCheckout ? Style::success("installed") : Style::warning("missing")) << "\n";
    std::cout << "    pre-merge-commit:  " << (preMergeCommit ? Style::success("installed") : Style::warning("missing")) << "\n";

    auto stagedFiles = ghost::git::Diff::getChangedFiles("--cached");
    auto unstagedFiles = ghost::git::Diff::getChangedFiles("");
    std::cout << "\n" << Style::bold(Style::blue("  Working Tree")) << "\n";
    std::cout << "    staged files:    " << Style::glow(std::to_string(stagedFiles.size()))
              << Style::dim("  (used by ghost check)") << "\n";
    std::cout << "    unstaged files:  " << Style::glow(std::to_string(unstagedFiles.size()))
              << Style::dim("  (not checked until staged)") << "\n";

    auto timeAgo = [](time_t ts) -> std::string {
        time_t now = std::time(nullptr);
        double diff = difftime(now, ts);
        if (diff < 0) diff = 0;
        if (diff < 5) return "just now";
        if (diff < 60) return std::to_string(static_cast<int>(diff)) + " secs ago";
        if (diff < 3600) return std::to_string(static_cast<int>(diff / 60)) + " mins ago";
        if (diff < 86400) return std::to_string(static_cast<int>(diff / 3600)) + " hrs ago";
        return std::to_string(static_cast<int>(diff / 86400)) + " days ago";
    };

    std::cout << "\n" << Style::bold(Style::blue("  Uncommitted Agent Sessions")) << "\n";
    std::cout << "    " << Style::dim("Completed agent edits waiting to be attached to the next commit.") << "\n";

    auto* db = ghost::persist::getRepoDb(repoRoot);
    if (!db) {
        std::cout << "    " << Style::dim("No Ghost session database yet. Run ghost init or install agent hooks to capture sessions.") << "\n";
    } else {
        auto sessions = db->loadSessions(true);
        sessions.erase(
            std::remove_if(sessions.begin(), sessions.end(),
                [&](const auto& session) { return !sessionBelongsToRepo(session, repoRoot); }),
            sessions.end()
        );
        std::string ghostDir = repoRoot + "/.git/ghost";
        bool hasPreState = fileExists(ghostDir + "/working.log");

        if (sessions.empty() && !hasPreState) {
            std::cout << "    " << Style::dim("No completed agent sessions waiting for commit.") << "\n";
        } else {
            // Sort by ts_start descending (newest first)
            std::sort(sessions.begin(), sessions.end(),
                [](const auto& a, const auto& b) { return a.ts_start > b.ts_start; });

            int totalAiAdditions = 0;
            int totalAiDeletions = 0;
            for (const auto& s : sessions) {
                totalAiAdditions += s.additions;
                totalAiDeletions += s.deletions;
            }

            std::cout << "    sessions:      " << Style::glow(std::to_string(sessions.size())) << "\n";
            std::cout << "    ai additions:  " << Style::success("+" + std::to_string(totalAiAdditions)) << "\n";
            std::cout << "    ai deletions:  " << Style::warning("-" + std::to_string(totalAiDeletions)) << "\n";

            // Show cumulative bar (AI-only since ghost doesn't track human edits)
            if (totalAiAdditions > 0 || totalAiDeletions > 0) {
                int total = totalAiAdditions + totalAiDeletions;
                int barWidth = 40;
                int aiChars = (total > 0) ? (totalAiAdditions * barWidth) / total : 0;
                std::string bar;
                for (int i = 0; i < barWidth; ++i) {
                    bar += (i < aiChars) ? "█" : "░";
                }
                int pct = (total > 0) ? std::min((totalAiAdditions * 100) / total, 100) : 0;
                std::cout << "\n    " << Style::bold("change mix") << Style::dim("  ") << bar
                          << "  " << Style::glow(std::to_string(pct) + "% additions") << "\n";
            }

            std::cout << "\n";
            for (const auto& s : sessions) {
                std::string agentModel = s.agent + "/" + s.model;
                auto files = extractSessionFiles(s.json_data, repoRoot);
                std::cout << "    " << Style::dim(timeAgo(s.ts_start))
                          << "  " << Style::success("+" + std::to_string(s.additions))
                          << "  " << Style::warning("-" + std::to_string(s.deletions))
                          << "  " << Style::glow(agentModel)
                          << "  " << Style::dim(std::to_string(files.size()) + " file" + (files.size() == 1 ? "" : "s")) << "\n";
                for (size_t i = 0; i < std::min<size_t>(files.size(), 3); ++i) {
                    std::cout << "       " << Style::dim(files[i]) << "\n";
                }
                if (files.size() > 3) {
                    std::cout << "       " << Style::dim("+" + std::to_string(files.size() - 3) + " more") << "\n";
                }
            }

            if (hasPreState) {
                std::cout << "\n    " << Style::warning("Open pre-tool snapshot present; run the agent post hook to record a session.") << "\n";
            }
        }
    }

    // Notes summary
    std::string headSha = ghost::git::Repo::getHead();
    std::cout << "\n" << Style::bold(Style::blue("  Committed Attribution (HEAD)")) << "\n";
    std::cout << "    " << Style::dim("This is what ghost audit reads for the latest commit.") << "\n";
    if (headSha.empty()) {
        std::cout << "    " << Style::dim("No commits yet") << "\n\n";
        return GHOST_EXIT_OK;
    }
    std::string note = ghost::git::Notes::show("refs/notes/ghost", headSha);
    std::cout << "    " << Style::violet(headSha.substr(0, 8)) << " ";
    if (!note.empty()) {
        auto parsed = ghost::note::NoteReader::parse(note);
        if (parsed.success) {
            int totalEntries = 0;
            for (const auto& e : parsed.entries) totalEntries++;
            std::cout << Style::success("ghost note present") << " " << Style::dim("(" + std::to_string(totalEntries) + " files)") << "\n";
        } else {
            std::cout << Style::warning("ghost note present but unparsable") << "\n";
        }
    } else {
        std::cout << Style::dim("no ghost note") << "\n";
    }

    std::cout << "\n";
    return GHOST_EXIT_OK;
}

static int handleCheck(int argc, char* argv[]) {
    std::string repoRoot = ghost::git::Repo::getRoot();
    if (repoRoot.empty()) {
        std::cerr << ghost::output::Style::error("Not in a git repository") << "\n";
        return GHOST_EXIT_NOT_IN_REPO;
    }
    std::error_code cwdEc;
    std::filesystem::current_path(repoRoot, cwdEc);

    bool jsonOutput = hasFlag(argc, argv, "--json") || hasFlag(argc, argv, "-j");
    std::string configRef = getArg(argc, argv, "--config-ref");
    logVerbose("checking staged changes");

    using namespace ghost::output;

    // Get staged diff stats
    auto stagedFiles = ghost::git::Diff::getChangedFiles("--cached");
    if (stagedFiles.empty()) {
        if (jsonOutput) {
            std::cout << "{\n";
            std::cout << "  \"scope\": \"staged_changes\",\n";
            std::cout << "  \"staged_files\": 0,\n";
            std::cout << "  \"total_additions\": 0,\n";
            std::cout << "  \"predicted_ai_additions\": 0,\n";
            std::cout << "  \"predicted_ai_percent\": 0,\n";
            std::cout << "  \"would_pass\": true,\n";
            std::cout << "  \"status\": \"NO_STAGED_CHANGES\",\n";
            std::cout << "  \"message\": \"No staged changes to check.\"\n";
            std::cout << "}\n";
            return GHOST_EXIT_OK;
        }
        std::cout << Style::header("Ghost Check");
        std::cout << "  " << Style::dim("Scope: staged changes only. It does not evaluate unstaged edits or committed history.") << "\n\n";
        std::cout << "  " << Style::warning("No staged changes to check") << "\n";
        std::cout << "  " << Style::dim("Run 'git add <files>' first, or use 'ghost status' to see uncommitted agent sessions.") << "\n";
        std::cout << "  " << Style::dim("Use 'ghost audit' after committing to verify durable git notes.") << "\n\n";
        return GHOST_EXIT_OK;
    }

    // Check for active checkpoint session
    bool hasActiveSession = fileExists(repoRoot + "/.git/ghost/working.log");
    std::string sessionAgent = "unknown";
    std::string sessionModel = "unknown";
    if (hasActiveSession) {
        auto preState = ghost::checkpoint::WorkingLog::loadPreState(repoRoot);
        if (preState.valid) {
            sessionAgent = preState.agent;
        }
        // Try to read current model
        std::string modelFile = repoRoot + "/.git/ghost/.current_model";
        if (fileExists(modelFile)) {
            std::ifstream mf(modelFile);
            std::getline(mf, sessionModel);
        }
    }

    std::vector<ghost::persist::Session> uncommittedSessions;
    auto* db = ghost::persist::getRepoDb(repoRoot);
    if (db) {
        uncommittedSessions = db->loadSessions(true);
        uncommittedSessions.erase(
            std::remove_if(uncommittedSessions.begin(), uncommittedSessions.end(),
                [&](const auto& session) { return !sessionBelongsToRepo(session, repoRoot); }),
            uncommittedSessions.end()
        );
        std::sort(uncommittedSessions.begin(), uncommittedSessions.end(),
            [](const auto& a, const auto& b) { return a.ts_start > b.ts_start; });
    }

    // Load ghost notes for HEAD (for predicting modifications to existing AI lines)
    std::string headSha = ghost::git::Repo::getHead();
    std::map<std::string, ghost::note::NoteReader::Result> ghostNotes;
    if (!headSha.empty()) {
        std::string rawNote = ghost::git::Notes::show("refs/notes/ghost", headSha);
        if (!rawNote.empty()) {
            ghostNotes[headSha] = ghost::note::NoteReader::parse(rawNote);
        }
    }

    // Compute predictions per file
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

        // Determine if this file is likely AI-authored based on uncommitted sessions,
        // an open pre-state, or existing attribution on HEAD.
        auto sessionIt = std::find_if(uncommittedSessions.begin(), uncommittedSessions.end(),
            [&](const auto& session) { return sessionTouchesFile(session, df.path, repoRoot); });
        if (sessionIt != uncommittedSessions.end()) {
            pred.predictedAiAdditions = df.additions;
            pred.reason = "matches uncommitted session: " + sessionIt->agent + "/" + sessionIt->model;
            pred.basis = "uncommitted_session";
        } else if (hasActiveSession) {
            pred.predictedAiAdditions = df.additions;
            pred.reason = "open pre-tool snapshot: " + sessionAgent + "/" + sessionModel;
            pred.basis = "open_checkpoint";
        } else {
            // No active session: check if file has existing AI attribution in HEAD
            if (ghostNotes.count(headSha)) {
                const auto& note = ghostNotes[headSha];
                bool hasAiHistory = false;
                for (const auto& entry : note.entries) {
                    if (entry.file_path == df.path) {
                        hasAiHistory = true;
                        break;
                    }
                }
                if (hasAiHistory) {
                    // File previously had AI lines; modifications likely still AI
                    pred.predictedAiAdditions = df.additions;
                    pred.reason = "file has prior AI attribution";
                    pred.basis = "head_note_history";
                } else {
                    pred.predictedAiAdditions = 0;
                    pred.reason = "no matching session or prior AI attribution";
                    pred.basis = "none";
                }
            } else {
                pred.predictedAiAdditions = 0;
                pred.reason = "no matching session or prior AI attribution";
                pred.basis = "none";
            }
        }
        predictedAiAdditions += pred.predictedAiAdditions;
        predictions.push_back(pred);
    }

    double aiPercent = totalAdditions > 0 ? (predictedAiAdditions * 100.0) / totalAdditions : 0.0;

    // Load config for threshold check
    auto cfg = configRef.empty()
        ? ghost::config::GhostConfigReader::load(repoRoot)
        : ghost::config::GhostConfigReader::loadFromRef(repoRoot, configRef);
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
        std::cout << "  \"basis\": \"uncommitted_sessions_then_open_checkpoint_then_head_notes\",\n";
        std::cout << "  \"staged_files\": " << stagedFiles.size() << ",\n";
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
            std::cout << "    {\"path\": \"" << p.path << "\", ";
            std::cout << "\"additions\": " << p.additions << ", ";
            std::cout << "\"deletions\": " << p.deletions << ", ";
            std::cout << "\"predicted_ai_additions\": " << p.predictedAiAdditions << ", ";
            std::cout << "\"basis\": \"" << p.basis << "\", ";
            std::cout << "\"reason\": \"" << p.reason << "\"}";
            if (i + 1 < predictions.size()) std::cout << ",";
            std::cout << "\n";
        }
        std::cout << "  ]\n";
        std::cout << "}\n";
    } else {
        std::cout << Style::header("Ghost Check");
        std::cout << "  " << Style::dim("Scope: staged changes only. Prediction uses uncommitted sessions, open checkpoints, then HEAD notes.") << "\n\n";

        auto v = Style::violet;
        auto d = Style::dim;

        std::cout << Style::bold(Style::blue("  Summary")) << "\n";
        std::cout << "    staged files:             " << Style::glow(std::to_string(stagedFiles.size())) << "\n";
        std::cout << "    uncommitted sessions:     " << Style::glow(std::to_string(uncommittedSessions.size())) << "\n";
        std::cout << "    staged additions:         " << Style::success("+" + std::to_string(totalAdditions)) << "\n";
        std::cout << "    predicted AI additions:   " << (predictedAiAdditions > 0 ? Style::success("+" + std::to_string(predictedAiAdditions)) : d("0")) << "\n";
        std::cout << "    predicted AI share:       " << v(std::to_string(static_cast<int>(aiPercent)) + "%") << "\n\n";

        std::cout << Style::bold(Style::blue("  Files")) << "\n";
        for (const auto& p : predictions) {
            int pct = p.additions > 0 ? std::min((p.predictedAiAdditions * 100) / p.additions, 100) : 0;
            std::cout << "    " << Style::padRight(Style::blue(p.path), 30);
            std::cout << Style::padRight(std::to_string(p.additions) + "+ " + std::to_string(p.deletions) + "-", 12);
            std::cout << Style::progressBar(pct, 100, 10) << " ";
            if (pct > 0) {
                std::cout << d(p.reason);
            } else {
                std::cout << d(p.reason);
            }
            std::cout << "\n";
        }

        std::cout << "\n" << Style::bold(Style::blue("  Policy Preview")) << "\n";
        std::cout << "    " << d("threshold " + std::to_string(cfg.threshold) + "%")
                  << "  |  ";
        if (wouldPass) {
            std::cout << Style::success(statusMsg) << " " << Style::success("✓");
        } else {
            std::cout << Style::error(statusMsg) << " " << Style::error("✗");
        }
        std::cout << "\n";
        std::cout << "    " << d("This is a pre-commit prediction. Run 'ghost audit' after committing to verify notes.") << "\n\n";
    }

    return wouldPass ? GHOST_EXIT_OK : GHOST_EXIT_BLOCKED;
}

static int handlePostCommit(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    std::string repoRoot = ghost::git::Repo::getRoot();
    std::string commitSha = ghost::git::Repo::getHead();
    if (repoRoot.empty() || commitSha.empty()) {
        std::cerr << ghost::output::Style::error("Not in a git repository") << "\n";
        return GHOST_EXIT_NOT_IN_REPO;
    }
    std::error_code cwdEc;
    std::filesystem::current_path(repoRoot, cwdEc);
    logVerbose("post-commit for: " + commitSha);
    return ghost::commit::PostCommit::run(repoRoot, commitSha);
}

static int handleCompletion(int argc, char* argv[]) {
    if (argc < 3) {
        ghost::cli::CommandRegistry::printHelp("completion");
        return GHOST_EXIT_ERROR;
    }
    std::string shell = argv[2];
    logVerbose("generating completions for: " + shell);
    
    if (shell == "bash") {
        std::cout << "_ghost_completion() {\n";
        std::cout << "  local cur=\"${COMP_WORDS[COMP_CWORD]}\"\n";
        std::cout << "  local cmds=\"";
        auto cmds = ghost::cli::CommandRegistry::getAllCommands();
        for (size_t i = 0; i < cmds.size(); ++i) {
            if (i > 0) std::cout << " ";
            std::cout << cmds[i];
        }
        std::cout << "\"\n";
        std::cout << "  COMPREPLY=( $(compgen -W \"$cmds\" -- $cur) )\n";
        std::cout << "}\n";
        std::cout << "complete -F _ghost_completion ghost\n";
    } else if (shell == "zsh") {
        std::cout << "#compdef ghost\n";
        std::cout << "_ghost() {\n";
        std::cout << "  local -a cmds=(";
        auto cmds = ghost::cli::CommandRegistry::getAllCommands();
        for (size_t i = 0; i < cmds.size(); ++i) {
            if (i > 0) std::cout << " ";
            std::cout << "\"" << cmds[i] << "\"";
        }
        std::cout << ")\n";
        std::cout << "  _describe 'ghost commands' cmds\n";
        std::cout << "}\n";
        std::cout << "compdef _ghost ghost\n";
    } else if (shell == "fish") {
        auto cmds = ghost::cli::CommandRegistry::getAllCommands();
        for (const auto& cmd : cmds) {
            std::cout << "complete -c ghost -f -a '" << cmd << "'\n";
        }
    } else {
        std::cerr << ghost::output::Style::error("Unsupported shell: " + shell) << "\n";
        std::cerr << "Supported: bash, zsh, fish\n";
        return GHOST_EXIT_ERROR;
    }
    return GHOST_EXIT_OK;
}

static int handleRewriteLog(int argc, char* argv[]) {
    std::string repoRoot = ghost::git::Repo::getRoot();
    if (repoRoot.empty()) {
        std::cerr << ghost::output::Style::error("Not in a git repository") << "\n";
        return GHOST_EXIT_NOT_IN_REPO;
    }

    // --stdin: read old-sha new-sha pairs from stdin (for post-rewrite hook)
    if (hasFlag(argc, argv, "--stdin")) {
        auto mappings = ghost::rewrite::RewriteLog::readStdinMappings();
        if (mappings.empty()) {
            return GHOST_EXIT_OK;
        }

        // Detect event type: if only one mapping and new is HEAD, it's amend
        // Otherwise it's rebase
        std::vector<std::string> oldShas, newShas;
        for (const auto& [oldSha, newSha] : mappings) {
            oldShas.push_back(oldSha);
            newShas.push_back(newSha);
        }

        bool isAmend = (mappings.size() == 1);
        if (isAmend) {
            ghost::rewrite::CommitAmendEvent ev;
            ev.original_commit = oldShas[0];
            ev.amended_commit_sha = newShas[0];
            ghost::rewrite::RewriteLog::append(repoRoot, ghost::rewrite::RewriteEventType::CommitAmend, ev.toJson());
            ghost::rewrite::Processor::processAmend(repoRoot, ev.original_commit, ev.amended_commit_sha);
        } else {
            ghost::rewrite::RebaseCompleteEvent ev;
            ev.original_commits = oldShas;
            ev.new_commits = newShas;
            ghost::rewrite::RewriteLog::append(repoRoot, ghost::rewrite::RewriteEventType::RebaseComplete, ev.toJson());
            ghost::rewrite::Processor::processRebase(repoRoot, ev.original_commits, ev.new_commits);
        }
        return GHOST_EXIT_OK;
    }

    // --event <type> --repo <path>: manual event logging
    std::string eventType = getArg(argc, argv, "--event");
    std::string repoArg = getArg(argc, argv, "--repo");
    if (!repoArg.empty()) repoRoot = repoArg;

    if (!eventType.empty()) {
        if (eventType == "merge") {
            ghost::rewrite::RewriteLog::append(repoRoot, ghost::rewrite::RewriteEventType::Merge, "{}");
        } else if (eventType == "checkout") {
            std::string prev = getArg(argc, argv, "--prev");
            std::string next = getArg(argc, argv, "--new");
            ghost::rewrite::Processor::detectStashPop(repoRoot, prev, next);
            ghost::rewrite::RewriteLog::append(repoRoot, ghost::rewrite::RewriteEventType::Stash, "{}");
        }
        return GHOST_EXIT_OK;
    }

    // Default: show recent rewrite events
    auto events = ghost::rewrite::RewriteLog::load(repoRoot, 20);
    if (events.empty()) {
        std::cout << "No rewrite events recorded.\n";
        return GHOST_EXIT_OK;
    }

    std::cout << "Recent rewrite events:\n";
    for (const auto& ev : events) {
        std::cout << "  " << ghost::rewrite::eventTypeToString(ev.type)
                  << " " << ev.json_payload << "\n";
    }
    return GHOST_EXIT_OK;
}

static int handleWorkingState(int argc, char* argv[]) {
    std::string repoRoot = ghost::git::Repo::getRoot();
    if (repoRoot.empty()) {
        std::cerr << ghost::output::Style::error("Not in a git repository") << "\n";
        return GHOST_EXIT_NOT_IN_REPO;
    }

    std::string repoArg = getArg(argc, argv, "--repo");
    if (!repoArg.empty()) repoRoot = repoArg;

    std::string key = getArg(argc, argv, "--key");
    if (key.empty()) key = "default";

    if (hasFlag(argc, argv, "--save")) {
        if (ghost::rewrite::WorkingState::save(repoRoot, key)) {
            std::cout << "Working state saved.\n";
            return GHOST_EXIT_OK;
        } else {
            std::cerr << "Failed to save working state.\n";
            return GHOST_EXIT_ERROR;
        }
    } else if (hasFlag(argc, argv, "--restore")) {
        if (ghost::rewrite::WorkingState::restore(repoRoot, key)) {
            std::cout << "Working state restored.\n";
            return GHOST_EXIT_OK;
        } else {
            std::cerr << "No saved working state found.\n";
            return GHOST_EXIT_ERROR;
        }
    } else if (hasFlag(argc, argv, "--clear")) {
        ghost::rewrite::WorkingState::clear(repoRoot, key);
        std::cout << "Working state cleared.\n";
        return GHOST_EXIT_OK;
    } else {
        bool exists = ghost::rewrite::WorkingState::exists(repoRoot, key);
        std::cout << "Working state '" << key << "': " << (exists ? "present" : "empty") << "\n";
        return GHOST_EXIT_OK;
    }
}
int main(int argc, char* argv[]) {
    // Check verbose first (global flag)
    // g_verbose will be set during command extraction below
    
    if (argc < 2) {
        ghost::cli::CommandRegistry::printGlobalHelp();
        return GHOST_EXIT_ERROR;
    }

    // Handle --version/-v and --help/-h before flag-skipping loop (only when no command follows)
    if (argc == 2) {
        std::string first = argv[1];
        if (first == "--version" || first == "-v") {
            ghost::cli::CommandRegistry::printVersion();
            return GHOST_EXIT_OK;
        }
        if (first == "--help" || first == "-h" || first == "-?" || first == "help") {
            ghost::cli::CommandRegistry::printGlobalHelp();
            return GHOST_EXIT_OK;
        }
    }
    if (argc == 3) {
        std::string first = argv[1];
        std::string second = argv[2];
        if (first == "--help" || first == "-h" || first == "-?" || first == "help") {
            std::string cmd = ghost::cli::CommandRegistry::resolveCommand(second);
            if (!cmd.empty()) {
                ghost::cli::CommandRegistry::printHelp(cmd);
                return GHOST_EXIT_OK;
            }
            std::cerr << ghost::output::Style::error("Unknown command: " + second) << "\n";
            printSuggestion(second);
            return GHOST_EXIT_ERROR;
        }
    }

    // Extract command, skipping global flags at argv[1]
    int cmdIndex = 1;
    while (cmdIndex < argc && std::string(argv[cmdIndex]).starts_with("-")) {
        if (std::string(argv[cmdIndex]) == "--verbose" || std::string(argv[cmdIndex]) == "-v") {
            g_verbose = true;
        }
        cmdIndex++;
    }
    
    std::string rawCommand;
    if (cmdIndex < argc) {
        rawCommand = argv[cmdIndex];
    }
    
    // Resolve command (with fuzzy matching)
    std::string command = ghost::cli::CommandRegistry::resolveCommand(rawCommand);
    
    if (command.empty()) {
        std::cerr << ghost::output::Style::error("Unknown command: " + rawCommand) << "\n";
        printSuggestion(rawCommand);
        return GHOST_EXIT_ERROR;
    }
    
    // Per-command --help
    if (hasFlag(argc, argv, "--help") || hasFlag(argc, argv, "-h") || hasFlag(argc, argv, "-?")) {
        ghost::cli::CommandRegistry::printHelp(command);
        return GHOST_EXIT_OK;
    }
    
    logVerbose("resolved command: " + command + " (from: " + rawCommand + ")");
    
    // Route to handlers
    if (command == "version") {
        ghost::cli::CommandRegistry::printVersion();
        return GHOST_EXIT_OK;
    } else if (command == "init") {
        return handleInit(argc, argv);
    } else if (command == "install") {
        return handleInstall(argc, argv);
    } else if (command == "uninstall") {
        return handleUninstall(argc, argv);
    } else if (command == "install-hooks") {
        return handleInstallHooks(argc, argv);
    } else if (command == "uninstall-hooks") {
        return handleUninstallHooks(argc, argv);
    } else if (command == "audit") {
        return handleAudit(argc, argv);
    } else if (command == "check") {
        return handleCheck(argc, argv);
    } else if (command == "blame") {
        return handleBlame(argc, argv);
    } else if (command == "show") {
        return handleShow(argc, argv);
    } else if (command == "stats") {
        return handleStats(argc, argv);
    } else if (command == "config") {
        return handleConfig(argc, argv);
    } else if (command == "banish") {
        return handleBanish(argc, argv);
    } else if (command == "doctor") {
        return handleDoctor(argc, argv);
    } else if (command == "status") {
        return handleStatus(argc, argv);
    } else if (command == "completion") {
        return handleCompletion(argc, argv);
    } else if (command == "post-commit") {
        return handlePostCommit(argc, argv);
    } else if (command == "rewrite-log") {
        return handleRewriteLog(argc, argv);
    } else if (command == "working-state") {
        return handleWorkingState(argc, argv);
    } else {
        std::cerr << ghost::output::Style::error("Command not yet implemented: " + command) << "\n";
        return GHOST_EXIT_ERROR;
    }
}
