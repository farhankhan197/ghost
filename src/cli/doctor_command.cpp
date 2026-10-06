#include "doctor_command.hpp"
#include "exit_codes.hpp"
#include "config/ghost_config.hpp"
#include "git/command.hpp"
#include "git/repo.hpp"
#include "hooks/agent_detector.hpp"
#include "hooks/agent_hooks.hpp"
#include "hooks/installer.hpp"
#include "output/style.hpp"
#include "output/ux.hpp"
#include "util/files.hpp"
#include "util/process.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace ghost {
namespace cli {
namespace {

bool hasFlag(int argc, char* argv[], const std::string& flag) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == flag) return true;
    }
    return false;
}

bool fileExists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

util::Process::Result runProcess(const std::string& executable, std::vector<std::string> args, const std::string& cwd = "") {
    util::Process::Command command;
    command.executable = executable;
    command.args = std::move(args);
    command.cwd = cwd;
    command.mergeStderr = true;
    return util::Process::capture(command);
}

std::string findExecutable(const std::string& name) {
#ifdef _WIN32
    auto result = runProcess("where", {name});
#else
    auto result = runProcess("which", {name});
#endif
    return result.ok() ? result.stdoutText : "";
}

struct DoctorCheck {
    std::string group;
    std::string label;
    bool ok = false;
    bool fixed = false;
    bool fixable = false;
    std::string detail;
    std::string fix;
};

struct CodexTrustCheck {
    bool relevant = false;
    bool ok = true;
    std::string detail;
    std::string fix;
};

std::filesystem::file_time_type modifiedTime(const std::filesystem::path& path) {
    std::error_code ec;
    auto time = std::filesystem::last_write_time(path, ec);
    return ec ? std::filesystem::file_time_type::min() : time;
}

CodexTrustCheck inspectCodexHookTrust() {
    CodexTrustCheck result;
    std::filesystem::path codexDir = std::filesystem::path(ghost::util::Files::homeDir()) / ".codex";
    std::filesystem::path hooksPath = codexDir / "hooks.json";
    std::filesystem::path configPath = codexDir / "config.toml";

    if (!fileExists(hooksPath.string())) return result;

    std::string hooksJson = ghost::util::Files::readText(hooksPath);
    bool hasNewHooks = hooksJson.find("ghost pre") != std::string::npos ||
        hooksJson.find("ghost\" pre") != std::string::npos;
    bool hasLegacyHooks = hooksJson.find("ghost-checkpoint") != std::string::npos;
    if (!hasNewHooks && !hasLegacyHooks) return result;

    result.relevant = true;
    result.detail = "Ghost hooks are trusted by Codex";
    result.fix = "Open Codex /hooks, trust Ghost hooks, then start a new Codex task";

    if (!fileExists(configPath.string())) {
        result.ok = false;
        result.detail = "Codex has no trusted hook state yet";
        return result;
    }

    std::string configToml = ghost::util::Files::readText(configPath);
    if (configToml.find("pre_tool_use:0:0") == std::string::npos ||
        configToml.find("post_tool_use:0:0") == std::string::npos) {
        result.ok = false;
        result.detail = "Codex has not trusted both Ghost hook phases";
        return result;
    }

    auto hooksTime = modifiedTime(hooksPath);
    auto configTime = modifiedTime(configPath);
    if (hooksTime != std::filesystem::file_time_type::min() &&
        configTime != std::filesystem::file_time_type::min() &&
        hooksTime > configTime) {
        result.ok = false;
        result.detail = "Ghost hooks changed after Codex last saved trusted hook state";
        return result;
    }

    return result;
}

void renderDoctorChecks(const std::vector<DoctorCheck>& checks, bool verbose) {
    using namespace ghost::output;
    std::string currentGroup;
    for (const auto& check : checks) {
        if (!verbose && check.ok && !check.fixed) continue;
        if (check.group != currentGroup) {
            currentGroup = check.group;
            std::cout << "\n  " << Style::bold(Style::violet(currentGroup)) << "\n";
        }
        std::string state = check.fixed ? "fixed" : (check.ok ? "ready" : "missing");
        std::string detail = check.ok || check.fixed ? check.detail : (check.fix.empty() ? check.detail : check.fix);
        std::cout << Ux::checkRow(state, check.label, detail);
    }
}

}

int doctor(int argc, char* argv[]) {
    using namespace ghost::output;

    bool verbose = hasFlag(argc, argv, "--verbose") || hasFlag(argc, argv, "-v");
    bool autoFix = hasFlag(argc, argv, "--fix") || hasFlag(argc, argv, "-f");

    std::string repoRoot = git::Repo::getRoot();
    if (repoRoot.empty()) {
        std::cout << Style::header("doctor");
        std::cout << "  " << Style::error("BROKEN") << Style::dim("  Not in a git repository") << "\n\n";
        return kExitError;
    }

    std::vector<DoctorCheck> checks;
    auto add = [&](DoctorCheck check) { checks.push_back(std::move(check)); };

    add({"Repository", "git repository", true, false, false, repoRoot, ""});

    std::string ghostPath = findExecutable("ghost");
    bool ghostInPath = !ghostPath.empty() && ghostPath.find("not found") == std::string::npos;
    add({"Repository", "Ghost in PATH", ghostInPath, false, false,
        ghostInPath ? ghostPath : "", "Run ghost init or add ~/.ghost/bin to PATH"});

    std::string ymlPath = repoRoot + "/ghost.yml";
    bool ymlExists = fileExists(ymlPath);
    bool ymlFixed = false;
    if (!ymlExists) {
        if (autoFix) {
            config::GhostConfigReader::save(repoRoot, "threshold", "80");
            ymlExists = true;
            ymlFixed = true;
        }
    }
    if (ymlExists) {
        auto cfg = config::GhostConfigReader::load(repoRoot);
        add({"Policy", "ghost.yml", true, ymlFixed, false,
            Ux::policySummary(cfg), ""});
    } else {
        add({"Policy", "ghost.yml", false, false, true,
            "", "Run ghost init to create owner policy"});
    }

    std::vector<std::string> repoHooks = {
        "post-commit",
        "pre-push",
        "post-rewrite",
        "post-merge",
        "post-checkout",
        "pre-merge-commit"
    };
    std::vector<std::string> missingHooks;
    for (const auto& hookName : repoHooks) {
        if (!fileExists(repoRoot + "/.git/hooks/" + hookName)) missingHooks.push_back(hookName);
    }
    bool hooksFixed = false;
    if (!missingHooks.empty() && autoFix) {
        hooksFixed = hooks::Installer::installRepo(repoRoot) == kExitOk;
        missingHooks.clear();
        for (const auto& hookName : repoHooks) {
            if (!fileExists(repoRoot + "/.git/hooks/" + hookName)) missingHooks.push_back(hookName);
        }
    }
    add({"Capture", "repo hooks", missingHooks.empty(), hooksFixed, true,
        missingHooks.empty() ? "commit attribution and pre-push enforcement" : "",
        "Run ghost doctor --fix to install missing repo hooks"});
    if (verbose) {
        for (const auto& hookName : repoHooks) {
            bool exists = fileExists(repoRoot + "/.git/hooks/" + hookName);
            add({"Capture", hookName + " hook", exists, false, true,
                repoRoot + "/.git/hooks/" + hookName,
                "Run ghost doctor --fix"});
        }
    }

    std::string remotePush = git::Command::capture(repoRoot, {"config", "--get-all", "remote.origin.push"}, "", true);
    bool hasNotesRef = (remotePush.find("refs/notes/ghost") != std::string::npos);
    bool notesFixed = false;
    if (!hasNotesRef && autoFix) {
        git::Command::run(repoRoot, {"config", "--add", "remote.origin.push", "+refs/notes/ghost:refs/notes/ghost"}, "", true);
        git::Command::run(repoRoot, {"config", "--add", "remote.origin.push", "+refs/notes/ghost-verified:refs/notes/ghost-verified"}, "", true);
        remotePush = git::Command::capture(repoRoot, {"config", "--get-all", "remote.origin.push"}, "", true);
        hasNotesRef = (remotePush.find("refs/notes/ghost") != std::string::npos);
        notesFixed = hasNotesRef;
    }
    add({"Notes", "notes push", hasNotesRef, notesFixed, true,
        hasNotesRef ? "refs/notes/ghost configured" : "",
        "Run ghost doctor --fix to configure notes push"});

    auto detected = hooks::AgentDetector::detectInstalled();
    if (detected.empty()) {
        add({"Agents", "agent capture hooks", true, false, false,
            "no supported agents detected", ""});
    } else {
        std::vector<std::string> missingAgents;
        for (const auto& agent : detected) {
            std::string agentDir = hooks::AgentDetector::getGlobalConfigDir(agent);
            bool hasHook = !agentDir.empty() && fileExists(agentDir);
            if (!hasHook) missingAgents.push_back(agent);
        }
        bool agentsFixed = false;
        if (!missingAgents.empty() && autoFix) {
            for (const auto& agent : missingAgents) {
                std::string agentDir = hooks::AgentDetector::getGlobalConfigDir(agent);
                bool hasHook = !agentDir.empty() && fileExists(agentDir);
                if (!hasHook) {
                    hooks::Installer::installBin();
                    hooks::AgentHooks::installForAgent(repoRoot, agent, true);
                }
            }
            missingAgents.clear();
            for (const auto& agent : detected) {
                std::string agentDir = hooks::AgentDetector::getGlobalConfigDir(agent);
                bool hasHook = !agentDir.empty() && fileExists(agentDir);
                if (!hasHook) missingAgents.push_back(agent);
            }
            agentsFixed = missingAgents.empty();
        }
        add({"Agents", "agent capture hooks", missingAgents.empty(), agentsFixed, true,
            missingAgents.empty() ? std::to_string(detected.size()) + " supported agent(s) ready" : "",
            "Run ghost doctor --fix to install global agent hooks"});
        if (std::find(detected.begin(), detected.end(), "codex") != detected.end()) {
            auto codexTrust = inspectCodexHookTrust();
            if (codexTrust.relevant) {
                add({"Agents", "Codex hook trust", codexTrust.ok, false, false,
                    codexTrust.detail,
                    codexTrust.fix});
            }
        }
        if (verbose) {
            for (const auto& agent : detected) {
                std::string agentDir = hooks::AgentDetector::getGlobalConfigDir(agent);
                bool hasHook = !agentDir.empty() && fileExists(agentDir);
                add({"Agents", agent + " global hook", hasHook, false, true,
                    agentDir,
                    "Run ghost doctor --fix"});
            }
        }
    }

    int ready = 0;
    int missing = 0;
    int fixable = 0;
    for (const auto& check : checks) {
        if (check.ok) ready++;
        else {
            missing++;
            if (check.fixable) fixable++;
        }
    }

    bool allOk = missing == 0;
    std::cout << Style::header("doctor");
    std::cout << "  " << (allOk ? Style::success("HEALTHY") : Style::warning("ACTION NEEDED"))
              << Style::dim("  ") << Style::glow(std::to_string(ready) + " ready")
              << Style::dim(" · ") << Style::warning(std::to_string(missing) + " missing")
              << Style::dim(" · ") << Style::muted(std::to_string(fixable) + " fix available") << "\n";

    renderDoctorChecks(checks, verbose || autoFix);
    if (!allOk && !autoFix) {
        std::cout << Ux::nextBlock({"ghost doctor --fix"});
    }
    std::cout << "\n";
    return allOk ? kExitOk : kExitError;
}

}
}
