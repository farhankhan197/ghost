#include "ghost_config.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <vector>
#include <cstdio>
#include <memory>
#include "../git/ref.hpp"

namespace ghost {
namespace config {

static std::string trim(const std::string& str) {
    size_t start = 0;
    while (start < str.size() && (str[start] == ' ' || str[start] == '\t')) start++;
    size_t end = str.size();
    while (end > start && (str[end - 1] == ' ' || str[end - 1] == '\t')) end--;
    return str.substr(start, end - start);
}

static std::string toLower(const std::string& str) {
    std::string lower = str;
    for (char& c : lower) c = static_cast<char>(std::tolower(c));
    return lower;
}

static std::string runGitCommand(const std::string& cmd) {
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) return "";
    std::string result;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe.get())) result += buffer;
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) result.pop_back();
    return result;
}

static GhostConfig parseConfigStream(std::istream& stream) {
    GhostConfig cfg;
    cfg.version = 1;
    cfg.required = false;
    cfg.threshold = 80;
    cfg.on_exceed = "block";
    cfg.pr_comment = true;
    cfg.untagged_policy = "human";
    cfg.unverified_policy = "warn";
    cfg.gitai_fallback = true;
    cfg.mode = "custom";
    cfg.policy_locked = false;

    std::string line;
    std::string lastListKey;
    while (std::getline(stream, line)) {
        std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;

        if (trimmed[0] == '-') {
            if (!lastListKey.empty()) {
                std::string item = trim(trimmed.substr(1));
                std::string lowerListKey = toLower(lastListKey);
                if (lowerListKey == "owners") {
                    cfg.owners.push_back(item);
                } else if (lowerListKey == "ignore") {
                    cfg.ignore.push_back(item);
                }
            }
            continue;
        }

        size_t colon = trimmed.find(':');
        if (colon == std::string::npos) continue;

        std::string key = trim(trimmed.substr(0, colon));
        std::string value = trim(trimmed.substr(colon + 1));

        if (value.empty()) {
            lastListKey = key;
            continue;
        }
        lastListKey.clear();

        std::string lowerKey = toLower(key);
        std::string lowerValue = toLower(value);

        if (lowerKey == "threshold") {
            try { cfg.threshold = std::stoi(value); } catch (...) {}
        } else if (lowerKey == "required") {
            cfg.required = (lowerValue == "true");
        } else if (lowerKey == "on_exceed") {
            cfg.on_exceed = lowerValue;
        } else if (lowerKey == "pr_comment") {
            cfg.pr_comment = (lowerValue == "true");
        } else if (lowerKey == "untagged" || lowerKey == "untagged_policy") {
            cfg.untagged_policy = lowerValue;
        } else if (lowerKey == "unverified" || lowerKey == "unverified_policy") {
            cfg.unverified_policy = lowerValue;
        } else if (lowerKey == "gitai_fallback" || lowerKey == "gitai_fb") {
            cfg.gitai_fallback = (lowerValue == "true");
        } else if (lowerKey == "version") {
            try { cfg.version = std::stoi(value); } catch (...) {}
        } else if (lowerKey == "owner") {
            cfg.owner = value;
        } else if (lowerKey == "mode") {
            cfg.mode = lowerValue;
        } else if (lowerKey == "locked" || lowerKey == "policy_locked") {
            cfg.policy_locked = (lowerValue == "true");
        }
    }

    if (!cfg.owner.empty() &&
        std::find(cfg.owners.begin(), cfg.owners.end(), cfg.owner) == cfg.owners.end()) {
        cfg.owners.push_back(cfg.owner);
    }

    return cfg;
}

GhostConfig GhostConfigReader::load(const std::string& repoRoot) {
    std::string path = repoRoot + "/ghost.yml";
    std::ifstream file(path);
    if (!file.is_open()) {
        GhostConfig cfg;
        cfg.version = 1;
        cfg.required = false;
        cfg.threshold = 80;
        cfg.on_exceed = "block";
        cfg.pr_comment = true;
        cfg.untagged_policy = "human";
        cfg.unverified_policy = "warn";
        cfg.gitai_fallback = true;
        cfg.mode = "custom";
        cfg.policy_locked = false;
        return cfg;
    }
    return parseConfigStream(file);
}

GhostConfig GhostConfigReader::loadFromRef(const std::string& repoRoot, const std::string& ref) {
    (void)repoRoot;
    if (!git::Ref::isSafeConfigRef(ref)) {
        GhostConfig cfg;
        cfg.version = 1;
        cfg.required = false;
        cfg.threshold = 80;
        cfg.on_exceed = "block";
        cfg.pr_comment = true;
        cfg.untagged_policy = "human";
        cfg.unverified_policy = "warn";
        cfg.gitai_fallback = true;
        cfg.mode = "custom";
        cfg.policy_locked = false;
        return cfg;
    }
#ifdef _WIN32
    std::string yaml = runGitCommand("git show " + ref + ":ghost.yml 2>nul");
#else
    std::string yaml = runGitCommand("git show " + ref + ":ghost.yml 2>/dev/null");
#endif
    if (yaml.empty()) {
        GhostConfig cfg;
        cfg.version = 1;
        cfg.required = false;
        cfg.threshold = 80;
        cfg.on_exceed = "block";
        cfg.pr_comment = true;
        cfg.untagged_policy = "human";
        cfg.unverified_policy = "warn";
        cfg.gitai_fallback = true;
        cfg.mode = "custom";
        cfg.policy_locked = false;
        return cfg;
    }
    std::istringstream stream(yaml);
    return parseConfigStream(stream);
}

static std::string normalizeValue(const std::string& key, const std::string& value) {
    std::string lowerKey = toLower(key);
    std::string lowerValue = toLower(value);
    if (lowerKey == "required" || lowerKey == "pr_comment" ||
        lowerKey == "gitai_fallback" || lowerKey == "gitai_fb" ||
        lowerKey == "locked" || lowerKey == "policy_locked") {
        return (lowerValue == "true" || lowerValue == "1" || lowerValue == "yes") ? "true" : "false";
    }
    if (lowerKey == "threshold" || lowerKey == "version") {
        try { std::stoi(value); return std::to_string(std::stoi(value)); } catch (...) {}
    }
    if (lowerKey == "mode" ||
        lowerKey == "on_exceed" ||
        lowerKey == "untagged" || lowerKey == "untagged_policy" ||
        lowerKey == "unverified" || lowerKey == "unverified_policy") {
        return lowerValue;
    }
    if (lowerKey == "owner") {
        return value;
    }
    return value;
}

bool GhostConfigReader::save(const std::string& repoRoot, const std::string& key, const std::string& value) {
    std::string path = repoRoot + "/ghost.yml";
    std::ifstream inFile(path);
    std::vector<std::string> lines;
    std::string line;
    bool found = false;

    while (std::getline(inFile, line)) {
        std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            lines.push_back(line);
            continue;
        }

        size_t colon = trimmed.find(':');
        if (colon == std::string::npos || trimmed[0] == '-') {
            lines.push_back(line);
            continue;
        }

        std::string existingKey = trim(trimmed.substr(0, colon));
        std::string existingLower = toLower(existingKey);
        std::string keyLower = toLower(key);
        bool sameKey = existingLower == keyLower ||
            ((existingLower == "untagged" || existingLower == "untagged_policy") &&
             (keyLower == "untagged" || keyLower == "untagged_policy")) ||
            ((existingLower == "unverified" || existingLower == "unverified_policy") &&
             (keyLower == "unverified" || keyLower == "unverified_policy"));
        if (sameKey) {
            std::string normalized = normalizeValue(key, value);
            lines.push_back(existingKey + ": " + normalized);
            found = true;
        } else {
            lines.push_back(line);
        }
    }
    inFile.close();

    if (!found) {
        std::string normalized = normalizeValue(key, value);
        lines.push_back(key + ": " + normalized);
    }

    std::ofstream outFile(path);
    if (!outFile.is_open()) return false;
    for (const auto& l : lines) {
        outFile << l << "\n";
    }
    outFile.close();
    return true;
}

bool GhostConfigReader::saveIgnore(const std::string& repoRoot, const std::vector<std::string>& patterns) {
    std::string path = repoRoot + "/ghost.yml";
    std::ifstream inFile(path);
    if (!inFile.is_open()) return false;

    std::vector<std::string> lines;
    std::string line;
    bool foundIgnore = false;
    bool pastIgnoreSection = false;

    while (std::getline(inFile, line)) {
        std::string trimmed = trim(line);

        // If we're past the ignore section, skip list items and their blank lines
        if (pastIgnoreSection) {
            // Check if this line starts a new key (not a list item, not a comment)
            size_t colon = trimmed.find(':');
            if (!trimmed.empty() && trimmed[0] != '#' && colon != std::string::npos && trimmed[0] != '-') {
                pastIgnoreSection = false;
                // Fall through to add this line normally
            } else {
                lines.push_back(line);
                continue;
            }
        }

        if (trimmed.empty() || trimmed[0] == '#') {
            lines.push_back(line);
            continue;
        }

        // List items under ignore — skip them
        if (trimmed[0] == '-') {
            if (foundIgnore) {
                continue; // skip existing list items
            }
            lines.push_back(line);
            continue;
        }

        size_t colon = trimmed.find(':');
        if (colon == std::string::npos) {
            lines.push_back(line);
            continue;
        }

        std::string existingKey = trim(trimmed.substr(0, colon));

        if (toLower(existingKey) == "ignore") {
            foundIgnore = true;
            pastIgnoreSection = true;
            // Write the ignore key with new patterns
            if (patterns.empty()) {
                lines.push_back(existingKey + ": []");
            } else {
                lines.push_back(existingKey + ":");
                for (const auto& p : patterns) {
                    lines.push_back("  - " + p);
                }
            }
            continue;
        }

        lines.push_back(line);
    }
    inFile.close();

    // If ignore key wasn't found, add it
    if (!foundIgnore && !patterns.empty()) {
        lines.push_back("ignore:");
        for (const auto& p : patterns) {
            lines.push_back("  - " + p);
        }
    }

    std::ofstream outFile(path);
    if (!outFile.is_open()) return false;
    for (const auto& l : lines) {
        outFile << l << "\n";
    }
    outFile.close();
    return true;
}

}
}
