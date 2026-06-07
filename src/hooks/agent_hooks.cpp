#include "agent_hooks.hpp"
#include "agent_detector.hpp"
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <iostream>
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

static std::string getGhostHooksDir() {
    return getHomeDir() + "/.ghost/hooks";
}

static std::string getHookScriptPath(const std::string& agent, const std::string& type) {
    return getGhostHooksDir() + "/" + agent + "/" + type;
}

static std::string getBinDir() {
    return getHomeDir() + "/.ghost/bin";
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
    if (agent == "codex") {
        cmd += " --codex-hook";
    }
    if (phase == "post") {
        cmd += " --model unknown";
    }
    return cmd;
}

static bool writeHookScripts(const std::string& agent) {
    std::string dir = getGhostHooksDir() + "/" + agent;
    std::error_code ec;
    fs::create_directories(dir, ec);

    std::string bin = getBinDir();
    std::string checkpoint = bin + "/ghost-checkpoint";

    // Pre hook script
    std::string prePath = dir + "/pre";
    std::ofstream preFile(prePath);
    if (!preFile.is_open()) return false;
    preFile << "#!/bin/sh\n";
    preFile << "\"" << checkpoint << "\" pre --agent " << agent << " 2>/dev/null || true\n";
    preFile << "exit 0\n";
    preFile.close();

    // Post hook script
    std::string postPath = dir + "/post";
    std::ofstream postFile(postPath);
    if (!postFile.is_open()) return false;
    postFile << "#!/bin/sh\n";
    postFile << "\"" << checkpoint << "\" post --agent " << agent << " --model unknown 2>/dev/null || true\n";
    postFile << "exit 0\n";
    postFile.close();

    // Make executable on Unix
    std::string chmod = "chmod +x \"" + prePath + "\" \"" + postPath + "\" 2>/dev/null";
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(chmod.c_str(), "r"), pclose);
    (void)pipe;

    return true;
}

static bool removeHookScripts(const std::string& agent) {
    std::string dir = getGhostHooksDir() + "/" + agent;
    std::error_code ec;
    fs::remove_all(dir, ec);
    return true;
}

// -- JSON config file manipulation helpers --

static std::string readFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return "";
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

static bool writeFile(const std::string& path, const std::string& content) {
    std::ofstream file(path);
    if (!file.is_open()) return false;
    file << content;
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
    std::string pre = getHookScriptPath(agent, "pre");
    std::string post = getHookScriptPath(agent, "post");
    return "{\n"
           "    \"PreToolUse\": {\n"
           "      \"matcher\": [{\"match\": \"Edit|Write|ApplyDiff\"}],\n"
           "      \"handler\": {\n"
           "        \"type\": \"command\",\n"
           "        \"command\": \"" + pre + "\"\n"
           "      }\n"
           "    },\n"
           "    \"PostToolUse\": {\n"
           "      \"matcher\": [{\"match\": \"Edit|Write|ApplyDiff\"}],\n"
           "      \"handler\": {\n"
           "        \"type\": \"command\",\n"
           "        \"command\": \"" + post + "\"\n"
           "      }\n"
           "    }\n"
           "  }";
}

static std::string cursorHooksJson(const std::string& agent) {
    std::string pre = getHookScriptPath(agent, "pre");
    std::string post = getHookScriptPath(agent, "post");
    return "{\n"
           "    \"version\": 1,\n"
           "    \"afterFileEdit\": [\n"
           "      {\n"
           "        \"command\": \"" + post + "\"\n"
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
           "          \"command\": \"" + jsonEscape(checkpointCommand(agent, phase, true)) + "\",\n"
           "          \"timeout\": 30\n"
           "        }";
}

static std::string antigravityHooksJson(const std::string& agent) {
    return "{\n"
           "  \"ghost\": {\n"
           "    \"enabled\": true,\n"
           "    \"PreToolUse\": [\n"
           "      {\n"
           "        \"matcher\": \"*\",\n"
           "        \"hooks\": [\n"
           "          " + antigravityHookHandlerJson(agent, "pre") + "\n"
           "        ]\n"
           "      }\n"
           "    ],\n"
           "    \"PostToolUse\": [\n"
           "      {\n"
           "        \"matcher\": \"*\",\n"
           "        \"hooks\": [\n"
           "          " + antigravityHookHandlerJson(agent, "post") + "\n"
           "        ]\n"
           "      }\n"
           "    ]\n"
           "  }\n"
           "}\n";
}

static const char* OPENCODE_PLUGIN_CONTENT = R"(function extractPath(input, output) {
  const sources = [output?.args, input?.args, output, input]
  for (const source of sources) {
    if (!source) continue
    for (const key of ["path", "file", "filePath", "file_path"]) {
      if (source[key] && typeof source[key] === "string") return source[key]
    }
    if (source.files && Array.isArray(source.files) && source.files.length > 0) {
      return source.files[0]
    }
  }
  return ""
}

function isTrackedTool(input, output) {
  const tool = input?.tool || output?.tool || input?.name || output?.name || ""
  return tool === "edit" || tool === "write" || tool === "apply_patch"
}

function normalizeModel(value) {
  if (!value) return ""
  if (typeof value === "string") {
    const parts = value.split("/")
    return parts[parts.length - 1] || value
  }
  if (typeof value === "object") {
    return normalizeModel(value.modelID || value.modelId || value.id || value.name || value.model)
  }
  return ""
}

function extractModelFromEvent(event) {
  if (!event) return ""
  const info = event.properties?.info || event.info || {}
  return normalizeModel(event.model) ||
    normalizeModel(event.properties?.model) ||
    normalizeModel(info.model) ||
    normalizeModel(info.modelID) ||
    normalizeModel(info.modelId)
}

function extractModelFromTool(input, output) {
  const sources = [output?.args, input?.args, output, input]
  for (const source of sources) {
    const model = normalizeModel(source?.model || source?.modelID || source?.modelId)
    if (model) return model
  }
  return ""
}

function detectModel() {
  try {
    const fs = require("fs")
    for (const home of homeCandidates()) {
      const modelPath = home + "/.ghost/.current_model"
      if (fs.existsSync(modelPath)) {
        const model = fs.readFileSync(modelPath, "utf8").trim()
        if (model) return model
      }
    }
  } catch (e) {}
  return ""
}

function homeCandidates() {
  const values = []
  const add = (value) => {
    if (value && typeof value === "string" && !values.includes(value)) values.push(value)
  }
  add(process.env.HOME)
  add(process.env.USERPROFILE)
  if (process.env.USERNAME) add("/mnt/c/Users/" + process.env.USERNAME)
  if (process.env.USER) add("/mnt/c/Users/" + process.env.USER)
  return values
}

function pathExists(filePath) {
  try {
    return require("fs").existsSync(filePath)
  } catch (e) {
    return false
  }
}

function resolveFilePath(filePath, directory, worktree) {
  if (!filePath) return ""
  try {
    const path = require("path")
    if (path.isAbsolute(filePath)) return filePath
    const base = (typeof directory === "string" && directory) ||
      (typeof worktree === "string" && worktree) ||
      process.cwd()
    return path.resolve(base, filePath)
  } catch (e) {
    return filePath
  }
}

export const GhostPlugin = async ({ $, directory, worktree }) => {
  let currentModel = detectModel() || "opencode"
  writeModelFile(currentModel)

  function getCheckpointPath() {
    const bins = []
    const addBin = (bin) => {
      if (bin && typeof bin === "string" && !bins.includes(bin)) bins.push(bin)
    }
    addBin(process.env.GHOST_BIN)
    for (const home of homeCandidates()) addBin(home + "/.ghost/bin")

    for (const bin of bins) {
      const unixPath = bin + "/ghost-checkpoint"
      const exePath = process.platform === "win32"
        ? bin.replace(/\//g, "\\") + "\\ghost-checkpoint.exe"
        : bin + "/ghost-checkpoint.exe"
      if (pathExists(unixPath)) return unixPath
      if (pathExists(exePath)) return exePath
    }

    const fallback = bins[0] || ((process.env.HOME || process.env.USERPROFILE || "") + "/.ghost/bin")
    return process.platform === "win32"
      ? fallback.replace(/\//g, "\\") + "\\ghost-checkpoint.exe"
      : fallback + "/ghost-checkpoint"
  }

  function writeModelFile(model) {
    const home = homeCandidates()[0] || ""
    const ghostDir = home + "/.ghost"
    const modelPath = home + "/.ghost/.current_model"
    try {
      const fs = require("fs")
      fs.mkdirSync(ghostDir, { recursive: true })
      if (model) {
        fs.writeFileSync(modelPath, model)
      } else {
        try { fs.unlinkSync(modelPath) } catch (e) {}
      }
    } catch (e) {}
  }

  return {
    event: async ({ event }) => {
      const model = extractModelFromEvent(event)
      if (model) currentModel = model
      else if (!currentModel) currentModel = detectModel() || "opencode"
      writeModelFile(currentModel)
    },
    "tool.execute.before": async (input, output) => {
      if (isTrackedTool(input, output)) {
        currentModel = extractModelFromTool(input, output) || currentModel || detectModel() || "opencode"
        writeModelFile(currentModel)
        const cp = getCheckpointPath()
        const filePath = resolveFilePath(extractPath(input, output), directory, worktree)
        if (filePath) {
          await $`${cp} pre --agent opencode --file ${filePath}`.quiet().catch(() => {})
        } else {
          await $`${cp} pre --agent opencode`.quiet().catch(() => {})
        }
      }
    },
    "tool.execute.after": async (input, output) => {
      if (isTrackedTool(input, output)) {
        currentModel = extractModelFromTool(input, output) || currentModel || detectModel() || "opencode"
        writeModelFile(currentModel)
        const cp = getCheckpointPath()
        const filePath = resolveFilePath(extractPath(input, output), directory, worktree)
        if (filePath) {
          await $`${cp} post --agent opencode --model ${currentModel} --file ${filePath}`.quiet().catch(() => {})
        } else {
          await $`${cp} post --agent opencode --model ${currentModel}`.quiet().catch(() => {})
        }
      }
    },
  }
}
)";

static bool installOpenCode(const std::string& configDir) {
    ensureDir(configDir);
    bool ok = writeFile(configDir + "/ghost.ts", OPENCODE_PLUGIN_CONTENT);

    fs::path dir(configDir);
    std::string name = dir.filename().string();
    if (name == "plugins" || name == "plugin") {
        std::error_code ec;
        fs::path legacy = dir.parent_path() / "plugin" / "ghost.ts";
        if (name == "plugins") {
            fs::remove(legacy, ec);
            fs::remove(dir.parent_path() / "plugin", ec);
        } else {
            fs::path canonical = dir.parent_path() / "plugins";
            ensureDir(canonical.string());
            ok = writeFile((canonical / "ghost.ts").string(), OPENCODE_PLUGIN_CONTENT) && ok;
            fs::remove(legacy, ec);
            fs::remove(dir.parent_path() / "plugin", ec);
        }
    }
    return ok;
}

static bool uninstallOpenCode(const std::string& configDir) {
    std::error_code ec;
    fs::remove(configDir + "/ghost.ts", ec);
    fs::path dir(configDir);
    std::string name = dir.filename().string();
    if (name == "plugins" || name == "plugin") {
        fs::path compat = dir.parent_path() / (name == "plugins" ? "plugin" : "plugins") / "ghost.ts";
        fs::remove(compat, ec);
    }
    return true;
}

static bool installClaude(const std::string& configDir) {
    if (!writeHookScripts("claude")) return false;
    std::string configPath = configDir + "/settings.json";
    ensureDir(configDir);

    std::string content = readFile(configPath);
    std::string hooks = claudeHooksJson("claude");
    std::string updated = setJsonKey(content, "hooks", hooks);
    return writeFile(configPath, updated);
}

static bool uninstallClaude(const std::string& configDir) {
    removeHookScripts("claude");
    std::string configPath = configDir + "/settings.json";
    std::string content = readFile(configPath);
    if (content.empty()) return true;
    std::string updated = removeJsonKey(content, "hooks");
    return writeFile(configPath, updated);
}

static bool installCursor(const std::string& configDir) {
    if (!writeHookScripts("cursor")) return false;
    std::string configPath = configDir + "/hooks.json";
    ensureDir(configDir);
    std::string content = readFile(configPath);
    if (content.empty() || content == "{}") {
        return writeFile(configPath, cursorHooksJson("cursor"));
    }
    return writeFile(configPath, cursorHooksJson("cursor"));
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
    return writeFile(configPath, "{\n  \"hooks\": " + hooks + "\n}\n");
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
    std::string content = readFile(configPath);
    std::string updated = setJsonKey(content, "hooks", hooks);
    if (!writeFile(configPath, updated)) return false;

    // Enable the current hooks feature flag in config.toml.
    std::string tomlPath = configDir + "/config.toml";
    std::string tomlContent = readFile(tomlPath);
    if (tomlContent.find("hooks") == std::string::npos && tomlContent.find("codex_hooks") == std::string::npos) {
        tomlContent += "\n[features]\nhooks = true\n";
        return writeFile(tomlPath, tomlContent);
    }
    return true;
}

static bool uninstallCodex(const std::string& configDir) {
    removeHookScripts("codex");
    std::string configPath = configDir + "/hooks.json";
    std::string content = readFile(configPath);
    if (!content.empty()) {
        std::string updated = removeJsonKey(content, "hooks");
        writeFile(configPath, updated);
    }
    return true;
}

static bool installAntigravity(const std::string& configDir) {
    ensureDir(configDir);
    std::string configPath = configDir + "/hooks.json";
    std::string content = readFile(configPath);
    std::string hooks = antigravityHooksJson("antigravity");
    std::string updated = setJsonKey(content, "hooks", hooks);
    return writeFile(configPath, updated);
}

static bool uninstallAntigravity(const std::string& configDir) {
    std::string configPath = configDir + "/hooks.json";
    std::string content = readFile(configPath);
    if (content.empty()) return true;
    std::string updated = removeJsonKey(content, "hooks");
    return writeFile(configPath, updated);
}

static bool installGemini(const std::string& configDir) {
    if (!writeHookScripts("gemini")) return false;
    std::string configPath = configDir + "/settings.json";
    ensureDir(configDir);
    std::string content = readFile(configPath);
    std::string hooks = claudeHooksJson("gemini");
    std::string updated = setJsonKey(content, "hooks", hooks);
    return writeFile(configPath, updated);
}

static bool uninstallGemini(const std::string& configDir) {
    removeHookScripts("gemini");
    std::string configPath = configDir + "/settings.json";
    std::string content = readFile(configPath);
    if (content.empty()) return true;
    std::string updated = removeJsonKey(content, "hooks");
    return writeFile(configPath, updated);
}

// -- Public API --

std::vector<std::string> AgentHooks::knownAgents() {
    return {"claude", "cursor", "copilot", "codex", "opencode", "antigravity", "gemini"};
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
    std::string configDir;
    if (global) {
        configDir = AgentDetector::getGlobalConfigDir(agent);
    } else {
        configDir = AgentDetector::getRepoConfigDir(agent, repoRoot);
    }

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
        configDir = AgentDetector::getRepoConfigDir(agent, repoRoot);
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
