#include <iostream>
#include <string>
#include "git/repo.hpp"
#include "git/notes.hpp"
#include "git/blame.hpp"
#include "note/reader.hpp"
#include "commit/post_commit.hpp"
#include "hooks/installer.hpp"
#include "hooks/agent_hooks.hpp"
#include "hooks/agent_detector.hpp"
#include "audit/auditor.hpp"
#include "audit/blame_overlay.hpp"
#include "output/report.hpp"
#include "config/ghost_config.hpp"

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

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "ghost - Git Hook for Origin Source Tracking\n";
        std::cout << "Usage: ghost <command> [options]\n";
        std::cout << "\nCommands:\n";
        std::cout << "  install              Install ghost in current repo\n";
        std::cout << "  install --global     Install ghost for all repos (opencode)\n";
        std::cout << "  install-bin          Copy binaries to ~/.ghost/bin\n";
        std::cout << "  install-hooks        Install hooks for detected AI agents\n";
        std::cout << "  install-hooks --agent <name>  Install hooks for a specific agent\n";
        std::cout << "  install-hooks --global       Install globally (default)\n";
        std::cout << "  install-hooks --repo         Install per-repo\n";
        std::cout << "  uninstall            Remove ghost from current repo\n";
        std::cout << "  uninstall --global   Remove global ghost plugin\n";
        std::cout << "  uninstall-hooks      Remove hooks for all AI agents\n";
        std::cout << "  uninstall-hooks --agent <name>  Remove hooks for a specific agent\n";
        std::cout << "  show <commit>        Show ghost note for commit\n";
        std::cout << "  blame <file>         Line-by-line attribution for a file\n";
        std::cout << "  blame <file> --json  JSON output\n";
        std::cout << "  audit                Run AI attribution audit (summary only)\n";
        std::cout << "  audit --all          Full per-commit breakdown for all commits\n";
        std::cout << "  audit --range <>..<> Audit a specific commit range\n";
        std::cout << "  audit --threshold <n> Override AI% threshold\n";
        std::cout << "  audit --json         JSON output\n";
        std::cout << "  stats                AI% stats for HEAD commit\n";
        std::cout << "  stats <sha1>..<sha2> Stats for a commit range\n";
        std::cout << "  stats --json         JSON output\n";
        std::cout << "  config               Show ghost.yml config\n";
        std::cout << "  config set <k> <v>   Set a config value\n";
        std::cout << "  post-commit          Run post-commit hook\n";
        std::cout << "  version              Print version\n";
        return 1;
    }

    std::string command = argv[1];

    if (command == "version") {
        std::cout << "ghost version 1.0.0\n";
    } else if (command == "install") {
        if (argc > 2 && std::string(argv[2]) == "--global") {
            return ghost::hooks::Installer::installGlobal();
        }
        if (argc > 2 && std::string(argv[2]) == "-bin") {
            return ghost::hooks::Installer::installBin();
        }
        std::string repoRoot = ghost::git::Repo::getRoot();
        if (repoRoot.empty()) {
            std::cerr << "Not in a git repository\n";
            return 1;
        }
        int binResult = ghost::hooks::Installer::installBin();
        if (binResult != 0) {
            std::cerr << "Warning: failed to install binaries, plugin may not work\n";
        }
        return ghost::hooks::Installer::installRepo(repoRoot);
    } else if (command == "uninstall") {
        if (argc > 2 && std::string(argv[2]) == "--global") {
            return ghost::hooks::Installer::uninstallGlobal();
        }
        std::string repoRoot = ghost::git::Repo::getRoot();
        if (repoRoot.empty()) {
            std::cerr << "Not in a git repository\n";
            return 1;
        }
        return ghost::hooks::Installer::uninstallRepo(repoRoot);
    } else if (command == "install-bin") {
        return ghost::hooks::Installer::installBin();
    } else if (command == "show") {
        if (argc < 3) {
            std::cerr << "Usage: ghost show <commit>\n";
            return 1;
        }
        std::string commit_sha = argv[2];
        std::string note = ghost::git::Notes::show("refs/notes/ghost", commit_sha);
        if (note.empty()) {
            std::cout << "No ghost note found for " << commit_sha << "\n";
        } else {
            auto result = ghost::note::NoteReader::parse(note);
            if (!result.success) {
                std::cout << "Failed to parse note: " << result.error << "\n";
                std::cout << "\nRaw note:\n" << note << "\n";
            } else {
                bool hasTerm = std::getenv("TERM") != nullptr && std::getenv("NO_COLOR") == nullptr;
                auto v = [&](const std::string& s) { return hasTerm ? "\033[38;5;141m" + s + "\033[0m" : s; };
                auto b = [&](const std::string& s) { return hasTerm ? "\033[38;5;75m" + s + "\033[0m" : s; };
                auto w = [&](const std::string& s) { return hasTerm ? "\033[38;5;231m" + s + "\033[0m" : s; };
                auto d = [&](const std::string& s) { return hasTerm ? "\033[2m\033[38;5;248m" + s + "\033[0m" : s; };
                for (const auto& entry : result.entries) {
                    std::cout << b(entry.file_path) << "\n";
                    auto it = result.sessions.find(entry.session_id);
                    if (it != result.sessions.end()) {
                        const auto& sess = it->second;
                        std::cout << "  " << d(entry.session_id)
                                  << "  " << d("lines") << " " << v(entry.ranges.toString())
                                  << "  " << d("(") << " " << w(sess.agent) << " " << d("/") << " " << w(sess.model) << " " << d(")") << "\n";
                    } else {
                        std::cout << "  " << d(entry.session_id)
                                  << "  " << d("lines") << " " << v(entry.ranges.toString()) << "\n";
                    }
                }
            }
        }
    } else if (command == "audit") {
        std::string repoRoot = ghost::git::Repo::getRoot();
        if (repoRoot.empty()) {
            std::cerr << "Not in a git repository\n";
            return 1;
        }
        std::string range = getArg(argc, argv, "--range");
        bool allMode = hasFlag(argc, argv, "--all");
        std::string thresholdStr = getArg(argc, argv, "--threshold");
        int threshold = -1;
        if (!thresholdStr.empty()) {
            try { threshold = std::stoi(thresholdStr); } catch (...) {}
        }
        bool jsonOutput = hasFlag(argc, argv, "--json");

        ghost::audit::AuditReport report;
        bool showDetail = false;

        if (!range.empty() || allMode) {
            if (!range.empty()) {
                report = ghost::audit::Auditor::run(repoRoot, range, threshold, jsonOutput);
            } else {
                report = ghost::audit::Auditor::run(repoRoot, "HEAD", threshold, jsonOutput);
            }
            showDetail = true;
        } else {
            std::vector<std::string> commitShas = ghost::audit::Auditor::getCommitsWithGhostNotes();
            report = ghost::audit::Auditor::runFromList(repoRoot, commitShas, threshold, jsonOutput);
            showDetail = false;
        }

        if (jsonOutput) {
            std::cout << ghost::output::Report::formatJSON(report.summary, report.policy);
        } else {
            std::cout << ghost::output::Report::formatCLI(report.summary, report.policy, showDetail);
        }
        return report.policy.blocked ? 1 : 0;
    } else if (command == "blame") {
        if (argc < 3) {
            std::cerr << "Usage: ghost blame <file> [--json]\n";
            return 1;
        }
        std::string filePath = argv[2];
        std::string repoRoot = ghost::git::Repo::getRoot();
        if (repoRoot.empty()) {
            std::cerr << "Not in a git repository\n";
            return 1;
        }
        std::string headSha = ghost::git::Repo::getHead();
        bool jsonOutput = hasFlag(argc, argv, "--json");

        auto blame = ghost::git::Blame::getLineAuthorMap(filePath);
        if (blame.empty()) {
            std::cout << "No blame data for " << filePath << "\n";
            return 0;
        }

        std::map<std::string, ghost::note::NoteReader::Result> ghostNotes;
        std::string rawNote = ghost::git::Notes::show("refs/notes/ghost", headSha);
        if (!rawNote.empty()) {
            ghostNotes[headSha] = ghost::note::NoteReader::parse(rawNote);
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
            auto g = [&](const std::string& s) { return hasTerm ? "\033[32m" + s + "\033[0m" : s; };
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
                ? (attribution.ai_lines * 100) / attribution.total_lines : 0;
            std::cout << "\n" << d(std::to_string(attribution.ai_lines) + "/" + std::to_string(attribution.total_lines))
                      << " AI lines (" << v(std::to_string(pct) + "%") << ")\n";
        }
    } else if (command == "stats") {
        std::string repoRoot = ghost::git::Repo::getRoot();
        if (repoRoot.empty()) {
            std::cerr << "Not in a git repository\n";
            return 1;
        }
        std::string range = "HEAD~1..HEAD";
        if (argc > 2 && std::string(argv[2])[0] != '-') {
            range = argv[2];
        }
        bool jsonOutput = hasFlag(argc, argv, "--json");

        auto report = ghost::audit::Auditor::run(repoRoot, range, -1, false);
        if (jsonOutput) {
            std::cout << "{\n";
            std::cout << "  \"total_commits\": " << report.summary.commits.size() << ",\n";
            std::cout << "  \"total_lines\": " << report.summary.total_lines << ",\n";
            std::cout << "  \"ai_lines\": " << report.summary.ai_lines << ",\n";
            std::cout << "  \"ai_percent\": " << (report.summary.total_lines > 0
                ? (report.summary.ai_lines * 100.0) / report.summary.total_lines : 0.0) << ",\n";
            std::cout << "  \"commits\": [\n";
            for (size_t i = 0; i < report.summary.commits.size(); ++i) {
                const auto& c = report.summary.commits[i];
                double cpct = c.total_lines > 0 ? (c.ai_lines * 100.0) / c.total_lines : 0.0;
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
            auto w = [&](const std::string& s) { return hasTerm ? "\033[38;5;231m" + s + "\033[0m" : s; };
            auto d = [&](const std::string& s) { return hasTerm ? "\033[2m\033[38;5;248m" + s + "\033[0m" : s; };
            for (const auto& c : report.summary.commits) {
                int cpct = c.total_lines > 0 ? (c.ai_lines * 100) / c.total_lines : 0;
                std::cout << "  " << b(c.commit_sha.substr(0, 8)) << "  "
                          << v(std::to_string(cpct) + "%") << " "
                          << d("(" + std::to_string(c.ai_lines) + "/" + std::to_string(c.total_lines) + " lines)") << "\n";
            }
            if (report.summary.commits.size() > 1) {
                int apct = report.summary.total_lines > 0
                    ? (report.summary.ai_lines * 100) / report.summary.total_lines : 0;
                std::cout << "\n  " << d("total") << "  " << v(std::to_string(apct) + "%") << " "
                          << d("(" + std::to_string(report.summary.ai_lines) + "/" + std::to_string(report.summary.total_lines) + " lines)") << "\n";
            }
        }
    } else if (command == "config") {
        std::string repoRoot = ghost::git::Repo::getRoot();
        if (repoRoot.empty()) {
            std::cerr << "Not in a git repository\n";
            return 1;
        }
        if (argc > 2 && std::string(argv[2]) == "set" && argc >= 5) {
            if (ghost::config::GhostConfigReader::save(repoRoot, argv[3], argv[4])) {
                std::cout << "Set " << argv[3] << " = " << argv[4] << "\n";
            } else {
                std::cerr << "Failed to write ghost.yml\n";
                return 1;
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
    } else if (command == "install-hooks") {
        std::string repoRoot = ghost::git::Repo::getRoot();
        if (repoRoot.empty()) {
            std::cerr << "Not in a git repository\n";
            return 1;
        }
        bool global = !hasFlag(argc, argv, "--repo");
        std::string specificAgent;
        for (int i = 2; i < argc - 1; ++i) {
            if (std::string(argv[i]) == "--agent" && i + 1 < argc) {
                specificAgent = argv[i + 1];
                break;
            }
        }

        if (!specificAgent.empty()) {
            if (!ghost::hooks::AgentHooks::installForAgent(repoRoot, specificAgent, global)) return 1;
        } else {
            if (!ghost::hooks::AgentHooks::installAll(repoRoot, global)) return 1;
        }
    } else if (command == "uninstall-hooks") {
        std::string repoRoot = ghost::git::Repo::getRoot();
        if (repoRoot.empty()) {
            std::cerr << "Not in a git repository\n";
            return 1;
        }
        bool global = !hasFlag(argc, argv, "--repo");
        std::string specificAgent;
        for (int i = 2; i < argc - 1; ++i) {
            if (std::string(argv[i]) == "--agent" && i + 1 < argc) {
                specificAgent = argv[i + 1];
                break;
            }
        }

        if (!specificAgent.empty()) {
            ghost::hooks::AgentHooks::uninstallForAgent(repoRoot, specificAgent, global);
        } else {
            ghost::hooks::AgentHooks::uninstallAll(repoRoot, global);
        }
    } else if (command == "post-commit") {
        std::string repoRoot = ghost::git::Repo::getRoot();
        std::string commitSha = ghost::git::Repo::getHead();
        if (repoRoot.empty() || commitSha.empty()) {
            std::cerr << "Not in a git repository\n";
            return 1;
        }
        return ghost::commit::PostCommit::run(repoRoot, commitSha);
    } else {
        std::cerr << "Unknown command: " << command << "\n";
        return 1;
    }

    return 0;
}
