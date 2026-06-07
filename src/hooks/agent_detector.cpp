#include "agent_detector.hpp"
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <memory>
#include <cstdio>

namespace fs = std::filesystem;

namespace ghost {
namespace hooks {

static std::string getHomeDir() {
    const char* home = std::getenv("USERPROFILE");
    if (!home) home = std::getenv("HOME");
    return home ? home : "";
}

static bool fileExists(const std::string& path) {
    std::error_code ec;
    return fs::exists(path, ec);
}

static bool commandExists(const std::string& cmd) {
    std::string check = "where " + cmd + " 2>nul || which " + cmd + " 2>/dev/null";
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(check.c_str(), "r"), pclose);
    if (!pipe) return false;
    char buffer[128];
    return fgets(buffer, sizeof(buffer), pipe.get()) != nullptr;
}

std::vector<std::string> AgentDetector::detectInstalled() {
    std::vector<std::string> agents;
    if (isInstalled("claude")) agents.push_back("claude");
    if (isInstalled("cursor")) agents.push_back("cursor");
    if (isInstalled("copilot")) agents.push_back("copilot");
    if (isInstalled("codex")) agents.push_back("codex");
    if (isInstalled("opencode")) agents.push_back("opencode");
    if (isInstalled("antigravity")) agents.push_back("antigravity");
    if (isInstalled("gemini")) agents.push_back("gemini");
    return agents;
}

bool AgentDetector::isInstalled(const std::string& agent) {
    std::string home = getHomeDir();
    if (agent == "claude") {
        return fileExists(home + "/.claude/settings.json") || commandExists("claude");
    }
    if (agent == "cursor") {
        return fileExists(home + "/.cursor/hooks.json") || commandExists("cursor");
    }
    if (agent == "copilot") {
        return commandExists("copilot");
    }
    if (agent == "codex") {
        return fileExists(home + "/.codex/config.toml") || commandExists("codex");
    }
    if (agent == "opencode") {
        return fileExists(home + "/.config/opencode/plugins/ghost.ts") ||
               fileExists(home + "/.config/opencode/plugin/ghost.ts") ||
               fileExists(home + "/.config/opencode/opencode.json") ||
               commandExists("opencode");
    }
    if (agent == "antigravity") {
        return fileExists(home + "/.gemini/config/hooks.json") ||
               fileExists(home + "/.gemini/antigravity-cli/hooks.json") ||
               commandExists("antigravity");
    }
    if (agent == "gemini") {
        return fileExists(home + "/.gemini/settings.json") || commandExists("gemini");
    }
    return false;
}

std::string AgentDetector::getGlobalConfigDir(const std::string& agent) {
    std::string home = getHomeDir();
    if (agent == "claude") return home + "/.claude";
    if (agent == "cursor") return home + "/.cursor";
    if (agent == "copilot") return home + "/.config/github-copilot";
    if (agent == "codex") return home + "/.codex";
    if (agent == "opencode") return home + "/.config/opencode/plugins";
    if (agent == "antigravity") return home + "/.gemini/config";
    if (agent == "gemini") return home + "/.gemini";
    return "";
}

std::string AgentDetector::getRepoConfigDir(const std::string& agent, const std::string& repoRoot) {
    if (agent == "claude") return repoRoot + "/.claude";
    if (agent == "cursor") return repoRoot + "/.cursor";
    if (agent == "copilot") return repoRoot + "/.github/hooks";
    if (agent == "codex") return repoRoot + "/.codex";
    if (agent == "opencode") return repoRoot + "/.opencode/plugins";
    if (agent == "antigravity") return repoRoot + "/.agents";
    if (agent == "gemini") return repoRoot + "/.gemini";
    return "";
}

}
}
