#include "agent_hooks.hpp"
#include "agent_detector.hpp"
#include "opencode_plugin.hpp"
#include "util/files.hpp"
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

namespace ghost {
namespace hooks {

static std::string getGhostHooksDir() {
    return util::Files::homeDir() + "/.ghost/hooks";
}

static std::string getHookScriptPath(const std::string& agent, const std::string& type) {
    return getGhostHooksDir() + "/" + agent + "/" + type;
}

static std::string getBinDir() {
    return util::Files::homeDir() + "/.ghost/bin";
}

static std::string jsonEscape(const std::string& value) {
    std::string out;
    for (char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c;
        }
    }
    return out;
}

static std::string quoteForShell(const std::string& value) {
    return "\"" + value + "\"";
}

static bool isWindowsBuild() {
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

static std::string checkpointPathUnix() {
    return getBinDir() + "/ghost-checkpoint";
}

static std::string checkpointPathWindows() {
    std::string path = getBinDir() + "/ghost-checkpoint.exe";
    for (char& c : path) {
        if (c == '/') c = '\\';
    }
    return path;
}

static std::string checkpointCommand(const std::string& agent, const std::string& phase, bool windows) {
    std::string exe = windows ? checkpointPathWindows() : checkpointPathUnix();
    std::string cmd = quoteForShell(exe) + " " + phase + " --agent " + agent;
    cmd += " --hook-json";
    if (phase == "post") {
        cmd += " --model unknown";
    }
    return cmd;
}

static std::string checkpointCommandForCurrentPlatform(const std::string& agent, const std::string& phase) {
    return checkpointCommand(agent, phase, isWindowsBuild());
}

static bool writeHookScripts(const std::string& agent) {
    std::string dir = getGhostHooksDir() + "/" + agent;
    std::error_code ec;
    fs::create_directories(dir, ec);

    std::string bin = getBinDir();
    std::string checkpoint = bin + "/ghost-checkpoint";

    std::string prePath = dir + "/pre";
    std::string preContent =
        "#!/bin/sh\n\"" + checkpoint + "\" pre --agent " + agent + " --hook-json 2>/dev/null || true\nexit 0\n";
    if (!util::Files::writeText(prePath, preContent)) return false;

    std::string postPath = dir + "/post";
    std::string postContent =
        "#!/bin/sh\n\"" + checkpoint + "\" post --agent " + agent + " --model unknown --hook-json 2>/dev/null || true\nexit 0\n";
    if (!util::Files::writeText(postPath, postContent)) return false;

    (void)util::Files::makeExecutable(prePath);
    (void)util::Files::makeExecutable(postPath);

    return true;
}

static bool removeHookScripts(const std::string& agent) {
    std::string dir = getGhostHooksDir() + "/" + agent;
    std::error_code ec;
    fs::remove_all(dir, ec);
    return true;
}

static void ensureDir(const std::string& path) {
    std::error_code ec;
    fs::create_directories(path, ec);
}

static std::string trimJson(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\n' || s[start] == '\r' || s[start] == '\t')) start++;
    size_t end = s.size();
    while (end > start && (s[end-1] == ' ' || s[end-1] == '\n' || s[end-1] == '\r' || s[end-1] == '\t')) end--;
    return s.substr(start, end - start);
}

// Add a "hooks" key to a JSON object string, or update existing hooks
// The hooksValue is inserted as the value for the "hooks" key
static std::string setJsonKey(const std::string& json, const std::string& key, const std::string& value) {
    std::string trimmed = trimJson(json);
    if (trimmed.empty() || trimmed == "{}") {
        return "{\n  \"" + key + "\": " + value + "\n}\n";
    }

    size_t closeBrace = trimmed.rfind('}');
    if (closeBrace == std::string::npos) {
        return "{\n  \"" + key + "\": " + value + "\n}\n";
    }

    // Check if key already exists
    std::string searchKey = "\"" + key + "\"";
    size_t keyPos = trimmed.find(searchKey);
    if (keyPos != std::string::npos && keyPos < closeBrace) {
        // Find the value for this key and replace it
        size_t colonPos = trimmed.find(':', keyPos + searchKey.size());
        if (colonPos != std::string::npos && colonPos < closeBrace) {
            // Find value start (after colon)
            size_t valStart = colonPos + 1;
            while (valStart < trimmed.size() && (trimmed[valStart] == ' ' || trimmed[valStart] == '\t')) valStart++;

            // Find value end - balanced braces/brackets
            size_t valEnd = valStart;
            if (trimmed[valStart] == '{') {
                int depth = 0;
                while (valEnd < trimmed.size()) {
                    if (trimmed[valEnd] == '{') depth++;
                    else if (trimmed[valEnd] == '}') { depth--; if (depth == 0) { valEnd++; break; } }
                    valEnd++;
                }
            } else if (trimmed[valStart] == '[') {
                int depth = 0;
                while (valEnd < trimmed.size()) {
                    if (trimmed[valEnd] == '[') depth++;
                    else if (trimmed[valEnd] == ']') { depth--; if (depth == 0) { valEnd++; break; } }
                    valEnd++;
                }
            } else {
                // String or simple value
                if (trimmed[valStart] == '"') {
                    valEnd = valStart + 1;
                    while (valEnd < trimmed.size()) {
                        if (trimmed[valEnd] == '"' && (valEnd == valStart + 1 || trimmed[valEnd-1] != '\\')) break;
                        valEnd++;
                    }
                    valEnd++;
                } else {
                    while (valEnd < trimmed.size() && trimmed[valEnd] != ',' && trimmed[valEnd] != '}' && trimmed[valEnd] != '\n') valEnd++;
                }
            }

            std::string before = trimmed.substr(0, keyPos);
            std::string after = trimmed.substr(valEnd);
            return before + "  \"" + key + "\": " + value + after;
        }
    }

    // Key doesn't exist, insert before closing brace
    std::string before = trimmed.substr(0, closeBrace);
    std::string after = trimmed.substr(closeBrace);

    // Ensure before ends with a newline
    while (!before.empty() && (before.back() == ' ' || before.back() == '\n')) before.pop_back();
    before += ",\n  \"" + key + "\": " + value + "\n";

    return before + after;
}

static std::string removeJsonKey(const std::string& json, const std::string& key) {
    std::string trimmed = trimJson(json);
    size_t closeBrace = trimmed.rfind('}');
    if (closeBrace == std::string::npos) return json;

    std::string searchKey = "\"" + key + "\"";
    size_t keyPos = trimmed.find(searchKey);
    if (keyPos == std::string::npos || keyPos > closeBrace) return json;

    // Find the comma before this key
    size_t lineStart = keyPos;
    while (lineStart > 0 && trimmed[lineStart-1] != '\n') lineStart--;

    // Find the end of this line's value (might span multiple lines with nested braces)
    size_t colonPos = trimmed.find(':', keyPos + searchKey.size());
    if (colonPos == std::string::npos || colonPos > closeBrace) return json;
    size_t valStart = colonPos + 1;
    while (valStart < trimmed.size() && trimmed[valStart] == ' ') valStart++;

    size_t valEnd = valStart;
    if (trimmed[valStart] == '{') {
        int depth = 0;
        while (valEnd < trimmed.size()) {
            if (trimmed[valEnd] == '{') depth++;
            else if (trimmed[valEnd] == '}') { depth--; if (depth == 0) { valEnd++; break; } }
            valEnd++;
        }
    } else if (trimmed[valStart] == '[') {
        int depth = 0;
        while (valEnd < trimmed.size()) {
            if (trimmed[valEnd] == '[') depth++;
            else if (trimmed[valEnd] == ']') { depth--; if (depth == 0) { valEnd++; break; } }
            valEnd++;
        }
    } else if (trimmed[valStart] == '"') {
        valEnd = valStart + 1;
        while (valEnd < trimmed.size()) {
            if (trimmed[valEnd] == '"' && trimmed[valEnd-1] != '\\') break;
            valEnd++;
        }
        valEnd++;
    } else {
        while (valEnd < trimmed.size() && trimmed[valEnd] != ',' && trimmed[valEnd] != '\n' && trimmed[valEnd] != '}') valEnd++;
    }

    // Find the end of this key-value pair (comma at end or comma before)
    // Remove from lineStart to valEnd, including trailing comma
    size_t removeEnd = valEnd;
    // Skip trailing whitespace and optional comma
    while (removeEnd < trimmed.size() && (trimmed[removeEnd] == ' ' || trimmed[removeEnd] == '\n' || trimmed[removeEnd] == '\r')) removeEnd++;
    bool hasTrailingComma = (removeEnd < trimmed.size() && trimmed[removeEnd] == ',');
    if (hasTrailingComma) removeEnd++;

    // Also check for leading comma
    size_t removeStart = lineStart;
    if (lineStart > 0) {
        size_t beforeLine = lineStart - 1;
        while (beforeLine > 0 && (trimmed[beforeLine] == ' ' || trimmed[beforeLine] == '\n' || trimmed[beforeLine] == '\r')) beforeLine--;
        if (trimmed[beforeLine] == ',') {
            removeStart = beforeLine;
        }
    }

    std::string result = trimmed.substr(0, removeStart) + trimmed.substr(removeEnd);

    // Clean up: remove empty braces
    std::string cleaned;
    for (size_t i = 0; i < result.size(); i++) {
        if (i + 2 < result.size() && result[i] == '{' && result[i+1] == '\n' && result[i+2] == '}') {
            cleaned += "{}";
            i += 2;
        } else {
            cleaned += result[i];
        }
    }

    return cleaned;
}

// -- Agent-specific hook config values --

static std::string claudeHooksJson(const std::string& agent) {
    std::string pre = checkpointCommandForCurrentPlatform(agent, "pre");
    std::string post = checkpointCommandForCurrentPlatform(agent, "post");
    return "{\n"
           "    \"PreToolUse\": [\n"
           "      {\n"
           "        \"matcher\": \"Write|Edit|MultiEdit|ApplyDiff\",\n"
           "        \"hooks\": [\n"
           "          {\"type\": \"command\", \"command\": \"" + jsonEscape(pre) + "\"}\n"
           "        ]\n"
           "      }\n"
           "    ],\n"
           "    \"PostToolUse\": [\n"
           "      {\n"
           "        \"matcher\": \"Write|Edit|MultiEdit|ApplyDiff\",\n"
           "        \"hooks\": [\n"
           "          {\"type\": \"command\", \"command\": \"" + jsonEscape(post) + "\"}\n"
           "        ]\n"
           "      }\n"
           "    ]\n"
           "  }";
}

static std::string cursorHooksJson(const std::string& agent) {
    std::string pre = checkpointCommandForCurrentPlatform(agent, "pre");
    std::string post = checkpointCommandForCurrentPlatform(agent, "post");
    return "{\n"
           "    \"version\": 1,\n"
           "    \"beforeFileEdit\": [\n"
           "      {\n"
           "        \"command\": \"" + jsonEscape(pre) + "\"\n"
           "      }\n"
           "    ],\n"
           "    \"afterFileEdit\": [\n"
           "      {\n"
           "        \"command\": \"" + jsonEscape(post) + "\"\n"
           "      }\n"
           "    ]\n"
           "  }";
}

static std::string codexHookHandlerJson(const std::string& agent, const std::string& phase, const std::string& status) {
    return "{\n"
           "          \"type\": \"command\",\n"
           "          \"command\": \"" + jsonEscape(checkpointCommand(agent, phase, false)) + "\",\n"
           "          \"commandWindows\": \"" + jsonEscape(checkpointCommand(agent, phase, true)) + "\",\n"
           "          \"timeout\": 30,\n"
           "          \"statusMessage\": \"" + jsonEscape(status) + "\"\n"
           "        }";
}

static std::string codexHooksJson(const std::string& agent) {
    return "{\n"
           "    \"PreToolUse\": [\n"
           "      {\n"
           "        \"matcher\": \"apply_patch|Edit|Write\",\n"
           "        \"hooks\": [\n"
           "          " + codexHookHandlerJson(agent, "pre", "Ghost checkpoint") + "\n"
           "        ]\n"
           "      }\n"
           "    ],\n"
           "    \"PostToolUse\": [\n"
           "      {\n"
           "        \"matcher\": \"apply_patch|Edit|Write\",\n"
           "        \"hooks\": [\n"
           "          " + codexHookHandlerJson(agent, "post", "Ghost attribution") + "\n"
           "        ]\n"
           "      }\n"
           "    ]\n"
           "  }";
}

static std::string antigravityHookHandlerJson(const std::string& agent, const std::string& phase) {
    return "{\n"
           "          \"type\": \"command\",\n"
           "          \"command\": \"" + jsonEscape(checkpointCommandForCurrentPlatform(agent, phase)) + "\",\n"
           "          \"commandWindows\": \"" + jsonEscape(checkpointCommand(agent, phase, true)) + "\",\n"
           "          \"timeout\": 30\n"
           "        }";
}

static std::string antigravityHooksJson(const std::string& agent) {
    return "{\n"
           "    \"PreToolUse\": [\n"
           "      {\n"
           "        \"matcher\": \"write_file|edit_file|replace|apply_patch|Write|Edit|MultiEdit\",\n"
           "        \"hooks\": [\n"
           "          " + antigravityHookHandlerJson(agent, "pre") + "\n"
           "        ]\n"
           "      }\n"
           "    ],\n"
           "    \"PostToolUse\": [\n"
           "      {\n"
           "        \"matcher\": \"write_file|edit_file|replace|apply_patch|Write|Edit|MultiEdit\",\n"
           "        \"hooks\": [\n"
           "          " + antigravityHookHandlerJson(agent, "post") + "\n"
           "        ]\n"
           "      }\n"
           "    ]\n"
           "  }";
}



static bool installOpenCode(const std::string& configDir) {
    ensureDir(configDir);
    bool ok = util::Files::writeText(configDir + "/ghost.ts", openCodePluginContent());

    fs::path dir(configDir);
    if (dir.filename().string() == "plugins") {
        std::error_code ec;
        fs::remove(dir.parent_path() / "plugin" / "ghost.ts", ec);
        fs::remove(dir.parent_path() / "plugin", ec);
    }
    return ok;
}

static bool uninstallOpenCode(const std::string& configDir) {
    std::error_code ec;
    fs::remove(configDir + "/ghost.ts", ec);
    return true;
}

static bool installClaude(const std::string& configDir) {
    std::string configPath = configDir + "/settings.json";
    ensureDir(configDir);

    std::string content = util::Files::readText(configPath);
    std::string hooks = claudeHooksJson("claude");
    std::string updated = setJsonKey(content, "hooks", hooks);
    return util::Files::writeText(configPath, updated);
}

static bool uninstallClaude(const std::string& configDir) {
    removeHookScripts("claude");
    std::string configPath = configDir + "/settings.json";
    std::string content = util::Files::readText(configPath);
    if (content.empty()) return true;
    std::string updated = removeJsonKey(content, "hooks");
    return util::Files::writeText(configPath, updated);
}

static bool installCursor(const std::string& configDir) {
    std::string configPath = configDir + "/hooks.json";
    ensureDir(configDir);
    std::string content = util::Files::readText(configPath);
    if (content.empty() || content == "{}") {
        return util::Files::writeText(configPath, cursorHooksJson("cursor"));
    }
    return util::Files::writeText(configPath, cursorHooksJson("cursor"));
}

static bool uninstallCursor(const std::string& configDir) {
    removeHookScripts("cursor");
    std::string configPath = configDir + "/hooks.json";
    std::error_code ec;
    if (fs::exists(configPath, ec)) {
        fs::remove(configPath, ec);
    }
    return true;
}

static bool installCopilot(const std::string& configDir) {
    if (!writeHookScripts("copilot")) return false;
    ensureDir(configDir);
    std::string configPath = configDir + "/ghost.json";
    std::string hooks = claudeHooksJson("copilot");
    return util::Files::writeText(configPath, "{\n  \"hooks\": " + hooks + "\n}\n");
}

static bool uninstallCopilot(const std::string& configDir) {
    removeHookScripts("copilot");
    std::string configPath = configDir + "/ghost.json";
    std::error_code ec;
    if (fs::exists(configPath, ec)) {
        fs::remove(configPath, ec);
    }
    return true;
}

static bool installCodex(const std::string& configDir) {
    ensureDir(configDir);
    std::string configPath = configDir + "/hooks.json";
    std::string hooks = codexHooksJson("codex");
    std::string content = util::Files::readText(configPath);
    std::string updated = setJsonKey(content, "hooks", hooks);
    if (!util::Files::writeText(configPath, updated)) return false;

    // Enable the current hooks feature flag in config.toml.
    std::string tomlPath = configDir + "/config.toml";
    std::string tomlContent = util::Files::readText(tomlPath);
    if (tomlContent.find("hooks") == std::string::npos && tomlContent.find("codex_hooks") == std::string::npos) {
        tomlContent += "\n[features]\nhooks = true\n";
        return util::Files::writeText(tomlPath, tomlContent);
    }
    return true;
}

static bool uninstallCodex(const std::string& configDir) {
    removeHookScripts("codex");
    std::string configPath = configDir + "/hooks.json";
    std::string content = util::Files::readText(configPath);
    if (!content.empty()) {
        std::string updated = removeJsonKey(content, "hooks");
        util::Files::writeText(configPath, updated);
    }
    return true;
}

static bool installAntigravity(const std::string& configDir) {
    ensureDir(configDir);
    std::string configPath = configDir + "/hooks.json";
    std::string content = util::Files::readText(configPath);
    std::string hooks = antigravityHooksJson("antigravity");
    std::string updated = setJsonKey(content, "hooks", hooks);
    return util::Files::writeText(configPath, updated);
}

static bool uninstallAntigravity(const std::string& configDir) {
    std::string configPath = configDir + "/hooks.json";
    std::string content = util::Files::readText(configPath);
    if (content.empty()) return true;
    std::string updated = removeJsonKey(content, "hooks");
    return util::Files::writeText(configPath, updated);
}

static bool installGemini(const std::string& configDir) {
    if (!writeHookScripts("gemini")) return false;
    std::string configPath = configDir + "/settings.json";
    ensureDir(configDir);
    std::string content = util::Files::readText(configPath);
    std::string hooks = claudeHooksJson("gemini");
    std::string updated = setJsonKey(content, "hooks", hooks);
    return util::Files::writeText(configPath, updated);
}

static bool uninstallGemini(const std::string& configDir) {
    removeHookScripts("gemini");
    std::string configPath = configDir + "/settings.json";
    std::string content = util::Files::readText(configPath);
    if (content.empty()) return true;
    std::string updated = removeJsonKey(content, "hooks");
    return util::Files::writeText(configPath, updated);
}

// -- Public API --

std::vector<std::string> AgentHooks::knownAgents() {
    return {"claude", "cursor", "copilot", "codex", "opencode", "antigravity", "gemini"};
}

std::vector<std::string> AgentHooks::defaultCaptureAgents() {
    return {"opencode", "codex", "claude", "cursor", "antigravity"};
}

std::string AgentHooks::displayName(const std::string& agent) {
    if (agent == "claude") return "Claude Code";
    if (agent == "cursor") return "Cursor";
    if (agent == "copilot") return "GitHub Copilot CLI";
    if (agent == "codex") return "OpenAI Codex CLI";
    if (agent == "opencode") return "OpenCode";
    if (agent == "antigravity") return "Google Antigravity";
    if (agent == "gemini") return "Google Gemini CLI";
    return agent;
}

bool AgentHooks::installForAgent(const std::string& repoRoot, const std::string& agent, bool global) {
    if (!global) {
        (void)repoRoot;
        std::cerr << "  Repo-level agent hooks are no longer installed. Use global hooks instead.\n";
        return false;
    }

    std::string configDir;
    configDir = AgentDetector::getGlobalConfigDir(agent);

    if (configDir.empty()) {
        std::cerr << "  Unknown agent: " << agent << "\n";
        return false;
    }

    bool result = false;
    if (agent == "claude") result = installClaude(configDir);
    else if (agent == "cursor") result = installCursor(configDir);
    else if (agent == "copilot") result = installCopilot(configDir);
    else if (agent == "codex") result = installCodex(configDir);
    else if (agent == "opencode") result = installOpenCode(configDir);
    else if (agent == "antigravity") result = installAntigravity(configDir);
    else if (agent == "gemini") result = installGemini(configDir);

    if (result) {
        std::cout << "  Installed hooks for " << displayName(agent)
                  << " (" << (global ? "global" : "repo") << ")\n";
    } else {
        std::cerr << "  Failed to install hooks for " << displayName(agent) << "\n";
    }
    return result;
}

bool AgentHooks::uninstallForAgent(const std::string& repoRoot, const std::string& agent, bool global) {
    std::string configDir;
    if (global) {
        configDir = AgentDetector::getGlobalConfigDir(agent);
    } else {
        // Keep repo-level uninstall as a legacy cleanup path for old installs.
        configDir = AgentDetector::getLegacyRepoConfigDir(agent, repoRoot);
    }

    if (configDir.empty()) {
        std::cerr << "  Unknown agent: " << agent << "\n";
        return false;
    }

    bool result = false;
    if (agent == "claude") result = uninstallClaude(configDir);
    else if (agent == "cursor") result = uninstallCursor(configDir);
    else if (agent == "copilot") result = uninstallCopilot(configDir);
    else if (agent == "codex") result = uninstallCodex(configDir);
    else if (agent == "opencode") result = uninstallOpenCode(configDir);
    else if (agent == "antigravity") result = uninstallAntigravity(configDir);
    else if (agent == "gemini") result = uninstallGemini(configDir);

    if (result) {
        std::cout << "  Uninstalled hooks for " << displayName(agent)
                  << " (" << (global ? "global" : "repo") << ")\n";
    } else {
        std::cerr << "  Failed to uninstall hooks for " << displayName(agent) << "\n";
    }
    return result;
}

bool AgentHooks::installAll(const std::string& repoRoot, bool global) {
    std::vector<std::string> installed = AgentDetector::detectInstalled();
    if (installed.empty()) {
        std::cout << "  No supported AI coding agents detected.\n";
        std::cout << "  Supported agents: ";
        auto agents = knownAgents();
        for (size_t i = 0; i < agents.size(); ++i) {
            std::cout << displayName(agents[i]);
            if (i + 1 < agents.size()) std::cout << ", ";
        }
        std::cout << "\n";
        std::cout << "  Use --agent <name> to install hooks for a specific agent.\n";
        return false;
    }

    bool allOk = true;
    for (const auto& agent : installed) {
        if (!installForAgent(repoRoot, agent, global)) allOk = false;
    }
    return allOk;
}

bool AgentHooks::uninstallAll(const std::string& repoRoot, bool global) {
    auto agents = knownAgents();
    bool allOk = true;
    for (const auto& agent : agents) {
        uninstallForAgent(repoRoot, agent, global);
        // Don't track failures for uninstall - best effort
    }
    return allOk;
}

}
}
