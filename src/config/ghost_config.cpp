#include "ghost_config.hpp"
#include <sstream>
#include <algorithm>
#include <vector>
#include "../util/files.hpp"
#include "../util/text.hpp"
#include "../git/ref.hpp"
#include "../git/engine.hpp"

namespace ghost {
namespace config {

static std::string trim(const std::string& str) {
    return util::Text::trim(str);
}

static std::string toLower(const std::string& str) {
    return util::Text::lower(str);
}

GhostConfig GhostConfigReader::defaults() {
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
    cfg.enforcement_scope = "final_diff";
    cfg.history_policy = "warn";
    return cfg;
}

static GhostConfig parseConfigStream(std::istream& stream) {
    GhostConfig cfg = GhostConfigReader::defaults();

    std::string line;
    std::string lastListKey;
    int currentTrustedSigner = -1;
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
                } else if (lowerListKey == "trusted_signers") {
                    cfg.trusted_signers.push_back(TrustedSigner{});
                    currentTrustedSigner = static_cast<int>(cfg.trusted_signers.size()) - 1;
                    size_t colon = item.find(':');
                    if (colon != std::string::npos) {
                        std::string key = toLower(trim(item.substr(0, colon)));
                        std::string value = trim(item.substr(colon + 1));
                        if (key == "name") cfg.trusted_signers[currentTrustedSigner].name = value;
                        else if (key == "email") cfg.trusted_signers[currentTrustedSigner].email = value;
                        else if (key == "github") cfg.trusted_signers[currentTrustedSigner].github = value;
                        else if (key == "ssh_key") cfg.trusted_signers[currentTrustedSigner].ssh_key = value;
                    }
                }
            }
            continue;
        }

        size_t colon = trimmed.find(':');
        if (colon == std::string::npos) continue;

        std::string key = trim(trimmed.substr(0, colon));
        std::string value = trim(trimmed.substr(colon + 1));

        if (lastListKey == "enforcement" && !value.empty() &&
            (line.find("  ") == 0 || line.find("\t") == 0)) {
            std::string lowerKey = toLower(key);
            std::string lowerValue = toLower(value);
            if (lowerKey == "scope") cfg.enforcement_scope = lowerValue;
            else if (lowerKey == "history") cfg.history_policy = lowerValue;
            continue;
        }

        if (lastListKey == "trusted_signers" && currentTrustedSigner >= 0 && !value.empty() &&
            (line.find("    ") == 0 || line.find("\t") == 0)) {
            std::string lowerKey = toLower(key);
            auto& signer = cfg.trusted_signers[static_cast<size_t>(currentTrustedSigner)];
            if (lowerKey == "name") signer.name = value;
            else if (lowerKey == "email") signer.email = value;
            else if (lowerKey == "github") signer.github = value;
            else if (lowerKey == "ssh_key") signer.ssh_key = value;
            continue;
        }

        if (value.empty()) {
            lastListKey = key;
            currentTrustedSigner = -1;
            continue;
        }
        lastListKey.clear();
        currentTrustedSigner = -1;

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
        } else if (lowerKey == "enforcement_scope" || lowerKey == "scope") {
            cfg.enforcement_scope = lowerValue;
        } else if (lowerKey == "history" || lowerKey == "history_policy") {
            cfg.history_policy = lowerValue;
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
    std::string content = util::Files::readText(path);
    if (content.empty() && !util::Files::exists(path)) {
        return defaults();
    }
    std::istringstream stream(content);
    return parseConfigStream(stream);
}

GhostConfig GhostConfigReader::loadFromRef(const std::string& repoRoot, const std::string& ref) {
    if (!git::Ref::isSafeConfigRef(ref)) {
        return defaults();
    }
    std::string yaml = git::Engine::showBlobAtRef(repoRoot, ref, "ghost.yml");
    if (yaml.empty()) {
        return defaults();
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
        lowerKey == "unverified" || lowerKey == "unverified_policy" ||
        lowerKey == "enforcement_scope" ||
        lowerKey == "history" || lowerKey == "history_policy") {
        return lowerValue;
    }
    if (lowerKey == "owner") {
        return value;
    }
    return value;
}

bool GhostConfigReader::save(const std::string& repoRoot, const std::string& key, const std::string& value) {
    std::string path = repoRoot + "/ghost.yml";
    std::istringstream inFile(util::Files::readText(path));
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
    if (!found) {
        std::string normalized = normalizeValue(key, value);
        lines.push_back(key + ": " + normalized);
    }

    std::ostringstream outFile;
    for (const auto& l : lines) {
        outFile << l << "\n";
    }
    return util::Files::writeText(path, outFile.str());
}

bool GhostConfigReader::saveIgnore(const std::string& repoRoot, const std::vector<std::string>& patterns) {
    std::string path = repoRoot + "/ghost.yml";
    std::string content = util::Files::readText(path);
    if (content.empty() && !util::Files::exists(path)) return false;
    std::istringstream inFile(content);

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
    // If ignore key wasn't found, add it
    if (!foundIgnore && !patterns.empty()) {
        lines.push_back("ignore:");
        for (const auto& p : patterns) {
            lines.push_back("  - " + p);
        }
    }

    std::ostringstream outFile;
    for (const auto& l : lines) {
        outFile << l << "\n";
    }
    return util::Files::writeText(path, outFile.str());
}

}
}
