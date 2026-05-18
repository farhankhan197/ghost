#include "ghost_config.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <vector>

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

GhostConfig GhostConfigReader::load(const std::string& repoRoot) {
    GhostConfig cfg;
    cfg.version = 1;
    cfg.required = false;
    cfg.threshold = 80;
    cfg.on_exceed = "block";
    cfg.pr_comment = true;
    cfg.untagged_policy = "human";
    cfg.unverified_policy = "warn";
    cfg.gitai_fallback = true;

    std::string path = repoRoot + "/ghost.yml";
    std::ifstream file(path);
    if (!file.is_open()) return cfg;

    std::string line;
    std::string lastListKey;
    while (std::getline(file, line)) {
        std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;

        if (trimmed[0] == '-') {
            if (!lastListKey.empty()) {
                cfg.ignore.push_back(trim(trimmed.substr(1)));
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
        } else if (lowerKey == "untagged_policy") {
            cfg.untagged_policy = lowerValue;
        } else if (lowerKey == "unverified_policy") {
            cfg.unverified_policy = lowerValue;
        } else if (lowerKey == "gitai_fallback") {
            cfg.gitai_fallback = (lowerValue == "true");
        } else if (lowerKey == "version") {
            try { cfg.version = std::stoi(value); } catch (...) {}
        }
    }

    return cfg;
}

static std::string normalizeValue(const std::string& key, const std::string& value) {
    std::string lowerKey = toLower(key);
    std::string lowerValue = toLower(value);
    if (lowerKey == "required" || lowerKey == "pr_comment" || lowerKey == "gitai_fallback") {
        return (lowerValue == "true" || lowerValue == "1" || lowerValue == "yes") ? "true" : "false";
    }
    if (lowerKey == "threshold" || lowerKey == "version") {
        try { std::stoi(value); return std::to_string(std::stoi(value)); } catch (...) {}
    }
    if (lowerKey == "on_exceed" || lowerKey == "untagged_policy" || lowerKey == "unverified_policy") {
        return lowerValue;
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
        if (toLower(existingKey) == toLower(key)) {
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
    for (size_t i = 0; i < lines.size(); ++i) {
        outFile << lines[i];
        if (i + 1 < lines.size()) outFile << "\n";
    }
    outFile.close();
    return true;
}

}
}
