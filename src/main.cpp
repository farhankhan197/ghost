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
#include <ctime>
#include "git/repo.hpp"
#include "git/notes.hpp"
#include "git/blame.hpp"
#include "git/diff.hpp"
#include "git/ref.hpp"
#include "note/reader.hpp"
#include "note/gitai_reader.hpp"
#include "commit/post_commit.hpp"
#include "commit/note_index.hpp"
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
#include "signing/ssh_signing.hpp"
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

static long long extractJsonNumberValue(const std::string& json, const std::string& key, size_t startAt = 0) {
    std::string search = "\"" + key + "\":";
    size_t pos = json.find(search, startAt);
    if (pos == std::string::npos) return 0;
    pos += search.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n')) {
        pos++;
    }
    size_t end = json.find_first_of(",}]", pos);
    if (end == std::string::npos) return 0;
    try {
        return std::stoll(json.substr(pos, end - pos));
    } catch (...) {
        return 0;
    }
}

static std::string escapeJsonString(const std::string& value) {
    std::ostringstream out;
    for (unsigned char c : value) {
        switch (c) {
            case '\\': out << "\\\\"; break;
            case '"': out << "\\\""; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20) {
                    out << "\\u"
                        << "00"
                        << "0123456789abcdef"[(c >> 4) & 0x0f]
                        << "0123456789abcdef"[c & 0x0f];
                } else {
                    out << static_cast<char>(c);
                }
                break;
        }
    }
    return out.str();
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

static ghost::note::LineRangeSet extractSessionRangesForFile(
    const std::string& jsonData,
    const std::string& filePath,
    const std::string& repoRoot
) {
    ghost::note::LineRangeSet result;
    std::string target = normalizeRepoPath(filePath, repoRoot);

    size_t pos = 0;
    while ((pos = jsonData.find("\"file_path\":", pos)) != std::string::npos) {
        std::string file = normalizeRepoPath(extractJsonStringValue(jsonData, "file_path", pos), repoRoot);
        std::string ranges = extractJsonStringValue(jsonData, "ranges", pos);
        if (file == target) {
            try {
                auto parsed = ranges.empty()
                    ? ghost::note::LineRangeSet::parse("")
                    : ghost::note::LineRangeSet::parse(ranges);
                result = result.unite(parsed);
            } catch (...) {
            }
        }
        pos += 12;
    }

    return result;
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

static std::string sessionFingerprintForDisplay(const ghost::persist::Session& session, const std::string& repoRoot) {
    std::vector<std::pair<std::string, std::string>> entries;
    size_t pos = 0;
    while ((pos = session.json_data.find("\"file_path\":", pos)) != std::string::npos) {
        std::string file = normalizeRepoPath(extractJsonStringValue(session.json_data, "file_path", pos), repoRoot);
        std::string ranges = extractJsonStringValue(session.json_data, "ranges", pos);
        if (!file.empty()) entries.push_back({file, ranges});
        pos += 12;
    }
    std::sort(entries.begin(), entries.end());

    std::ostringstream out;
    out << session.agent << "|"
        << session.model << "|"
        << session.author << "|"
        << session.ts_start << "|"
        << session.ts_end << "|";
    for (const auto& [path, ranges] : entries) {
        out << path << ":" << ranges << ";";
    }
    return out.str();
}

static void normalizePendingSessions(std::vector<ghost::persist::Session>& sessions, const std::string& repoRoot) {
    sessions.erase(
        std::remove_if(sessions.begin(), sessions.end(),
            [&](const auto& session) { return !sessionBelongsToRepo(session, repoRoot); }),
        sessions.end()
    );

    std::vector<ghost::persist::Session> unique;
    std::set<std::string> seenIds;
    std::set<std::string> seenFingerprints;
    for (const auto& session : sessions) {
        if (!session.session_id.empty() && !seenIds.insert(session.session_id).second) continue;

        std::string fingerprint = sessionFingerprintForDisplay(session, repoRoot);
        if (!fingerprint.empty() && !seenFingerprints.insert(fingerprint).second) continue;

        unique.push_back(session);
    }
    sessions = std::move(unique);
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

static bool shouldIgnorePath(const std::string& filePath, const std::vector<std::string>& ignorePatterns) {
    for (const auto& pattern : ignorePatterns) {
        if (pattern.empty()) continue;
        if (pattern.back() == '/') {
            std::string dirPrefix = pattern.substr(0, pattern.size() - 1);
            if (filePath == dirPrefix ||
                filePath.rfind(dirPrefix + "/", 0) == 0 ||
                filePath.find("/" + dirPrefix + "/") != std::string::npos) {
                return true;
            }
        } else if (pattern.front() == '*') {
            std::string suffix = pattern.substr(1);
            if (filePath.size() >= suffix.size() &&
                filePath.compare(filePath.size() - suffix.size(), suffix.size(), suffix) == 0) {
                return true;
            }
        } else if (filePath == pattern || filePath.find(pattern) != std::string::npos) {
            return true;
        }
    }
    return false;
}

static std::string lowerString(const std::string& value) {
    std::string lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return lower;
}

static std::string trimString(const std::string& value) {
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) start++;
    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) end--;
    return value.substr(start, end - start);
}

static bool isProtectedPolicyKey(const std::string& key) {
    static const std::set<std::string> protectedKeys = {
        "owner",
        "owners",
        "locked",
        "policy_locked",
        "mode",
        "required",
        "threshold",
        "on_exceed",
        "pr_comment",
        "untagged",
        "untagged_policy",
        "unverified",
        "unverified_policy",
        "gitai_fb",
        "gitai_fallback",
        "ignore"
    };
    return protectedKeys.count(lowerString(key)) > 0;
}

static bool currentUserOwnsPolicy(const ghost::config::GhostConfig& cfg, std::string& currentUser) {
    currentUser = ghost::git::Repo::getUserEmail();
    if (currentUser.empty()) return false;
    if (!cfg.owner.empty() && currentUser == cfg.owner) return true;
    return std::find(cfg.owners.begin(), cfg.owners.end(), currentUser) != cfg.owners.end();
}

static bool requirePolicyOwner(const ghost::config::GhostConfig& cfg, const std::string& action) {
    if (cfg.owner.empty() && cfg.owners.empty()) return true;

    std::string currentUser;
    if (currentUserOwnsPolicy(cfg, currentUser)) return true;

    std::string ownerLabel = cfg.owner;
    if (ownerLabel.empty() && !cfg.owners.empty()) ownerLabel = cfg.owners.front();
    if (cfg.owners.size() > 1) ownerLabel += " and " + std::to_string(cfg.owners.size() - 1) + " more";

    std::cerr << ghost::output::Style::error("Only the repo owner (" + ownerLabel + ") can " + action + ".\n")
              << ghost::output::Style::dim("  Current git user: " + (currentUser.empty() ? "unknown" : currentUser) + "\n")
              << ghost::output::Style::dim("  Run 'ghost policy' to inspect owner-controlled enforcement.\n");
    return false;
}

static bool requirePolicyUnlocked(const ghost::config::GhostConfig& cfg, const std::string& action) {
    if (!cfg.policy_locked) return true;
    std::cerr << ghost::output::Style::error("Policy is locked; cannot " + action + ".\n")
              << ghost::output::Style::dim("  Run 'ghost policy unlock --force' as an owner before changing protected policy.\n");
    return false;
}

struct PolicyModeDefaults {
    std::string mode;
    bool required;
    int threshold;
    std::string onExceed;
    std::string unverifiedPolicy;
};

static bool getPolicyModeDefaults(const std::string& mode, PolicyModeDefaults& defaults) {
    std::string normalized = lowerString(mode);
    if (normalized == "permissive") {
        defaults = {"permissive", false, 100, "warn", "warn"};
        return true;
    }
    if (normalized == "transparent") {
        defaults = {"transparent", true, 80, "warn", "warn"};
        return true;
    }
    if (normalized == "restrictive") {
        defaults = {"restrictive", true, 20, "block", "block"};
        return true;
    }
    if (normalized == "locked") {
        defaults = {"locked", true, 0, "block", "block"};
        return true;
    }
    return false;
}

static bool applyPolicyMode(const std::string& repoRoot, const std::string& mode) {
    PolicyModeDefaults defaults;
    if (!getPolicyModeDefaults(mode, defaults)) return false;
    bool ok = true;
    ok = ghost::config::GhostConfigReader::save(repoRoot, "mode", defaults.mode) && ok;
    ok = ghost::config::GhostConfigReader::save(repoRoot, "required", defaults.required ? "true" : "false") && ok;
    ok = ghost::config::GhostConfigReader::save(repoRoot, "threshold", std::to_string(defaults.threshold)) && ok;
    ok = ghost::config::GhostConfigReader::save(repoRoot, "on_exceed", defaults.onExceed) && ok;
    ok = ghost::config::GhostConfigReader::save(repoRoot, "unverified", defaults.unverifiedPolicy) && ok;
    return ok;
}

static bool configureNotesRefs() {
    std::string existing = execCommand("git config --get-all remote.origin.push 2>&1");
    auto addOnce = [&](const std::string& ref) {
        if (existing.find(ref) == std::string::npos) {
            execCommand("git config --add remote.origin.push " + ref + " 2>&1");
        }
    };
    addOnce("refs/notes/ghost");
    addOnce("refs/notes/ghost-verified");
    addOnce("refs/notes/ghost-signatures");
    return true;
}

static std::string hashFile(const std::string& path) {
    if (path.empty() || !fileExists(path)) return "";
    return execCommand("git hash-object \"" + path + "\" 2>&1");
}

static std::string hashText(const std::string& repoRoot, const std::string& content) {
    std::error_code ec;
    std::filesystem::path tmpDir = std::filesystem::path(repoRoot) / ".git" / "ghost";
    std::filesystem::create_directories(tmpDir, ec);
    std::filesystem::path tmpPath = tmpDir / ("hash-" + std::to_string(std::time(nullptr)) + ".txt");
    {
        std::ofstream out(tmpPath);
        if (!out.is_open()) return "";
        out << content;
    }
    std::string digest = hashFile(tmpPath.string());
    std::filesystem::remove(tmpPath, ec);
    return digest;
}

static std::map<std::string, std::string> parseSimpleSignature(const std::string& content) {
    std::map<std::string, std::string> result;
    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
        while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ')) value.pop_back();
        result[key] = value;
    }
    return result;
}

static long long parseSignatureTs(const std::map<std::string, std::string>& sig) {
    auto it = sig.find("ts");
    if (it == sig.end()) return 0;
    try {
        return std::stoll(it->second);
    } catch (...) {
        return 0;
    }
}

static bool writeTextFileIfMissing(const std::filesystem::path& path, const std::string& content, bool force) {
    std::error_code ec;
    if (!force && std::filesystem::exists(path, ec)) return true;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path);
    if (!file.is_open()) return false;
    file << content;
    return true;
}

static std::string inferGitHubOwnerFromOrigin() {
    std::string url = execCommand("git remote get-url origin 2>&1");
    if (url.empty()) return "";
    size_t githubPos = url.find("github.com");
    if (githubPos == std::string::npos) return "";

    std::string rest = url.substr(githubPos + std::string("github.com").size());
    while (!rest.empty() && (rest[0] == ':' || rest[0] == '/' || rest[0] == '\\')) {
        rest.erase(rest.begin());
    }
    size_t slash = rest.find_first_of("/\\");
    if (slash == std::string::npos) return "";
    std::string owner = rest.substr(0, slash);
    if (owner.empty()) return "";
    return "@" + owner;
}

struct GitHubRepoSlug {
    std::string owner;
    std::string repo;
};

static GitHubRepoSlug inferGitHubRepoFromOrigin() {
    GitHubRepoSlug slug;
    std::string url = execCommand("git remote get-url origin 2>&1");
    if (url.empty()) return slug;
    size_t githubPos = url.find("github.com");
    if (githubPos == std::string::npos) return slug;

    std::string rest = url.substr(githubPos + std::string("github.com").size());
    while (!rest.empty() && (rest[0] == ':' || rest[0] == '/' || rest[0] == '\\')) {
        rest.erase(rest.begin());
    }
    size_t slash = rest.find_first_of("/\\");
    if (slash == std::string::npos) return slug;
    slug.owner = rest.substr(0, slash);
    std::string repoPart = rest.substr(slash + 1);
    size_t nextSlash = repoPart.find_first_of("/\\");
    if (nextSlash != std::string::npos) repoPart = repoPart.substr(0, nextSlash);
    if (repoPart.size() > 4 && repoPart.substr(repoPart.size() - 4) == ".git") {
        repoPart = repoPart.substr(0, repoPart.size() - 4);
    }
    slug.repo = repoPart;
    return slug;
}

static std::string normalizeIdentityToken(const std::string& value) {
    std::string out = trimString(value);
    if (!out.empty() && out[0] == '@') out.erase(out.begin());
    return lowerString(out);
}

static bool isSafeGitHubSlugToken(const std::string& value) {
    if (value.empty()) return false;
    for (char c : value) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.') continue;
        return false;
    }
    return true;
}

static std::string currentGitHubLogin() {
    std::string login = execCommand("gh api user --jq .login 2>&1");
    if (!login.empty() &&
        login.find("not found") == std::string::npos &&
        login.find("not recognized") == std::string::npos &&
        login.find("Not recognized") == std::string::npos &&
        login.find("error") == std::string::npos &&
        login.find("ERROR") == std::string::npos) {
        return trimString(login);
    }
    login = execCommand("git config github.user 2>&1");
    if (!login.empty() && login.find("not found") == std::string::npos) {
        return trimString(login);
    }
    return "";
}

static bool hasGitHubMaintainerAccess(const GitHubRepoSlug& slug, const std::string& login) {
    if (slug.owner.empty() || slug.repo.empty() || login.empty()) return false;
    if (!isSafeGitHubSlugToken(slug.owner) ||
        !isSafeGitHubSlugToken(slug.repo) ||
        !isSafeGitHubSlugToken(login)) {
        return false;
    }
    if (normalizeIdentityToken(slug.owner) == normalizeIdentityToken(login)) return true;
    std::string permission = execCommand(
        "gh api repos/" + slug.owner + "/" + slug.repo +
        "/collaborators/" + login + "/permission --jq .permission 2>&1"
    );
    permission = normalizeIdentityToken(permission);
    return permission == "admin" || permission == "maintain";
}

static std::string currentGitUserName() {
    return trimString(execCommand("git config user.name 2>&1"));
}

static bool currentUserMatchesGhostOwner(const ghost::config::GhostConfig& cfg) {
    std::string email = normalizeIdentityToken(ghost::git::Repo::getUserEmail());
    std::string name = normalizeIdentityToken(currentGitUserName());
    std::string login = normalizeIdentityToken(currentGitHubLogin());

    for (const auto& owner : cfg.owners) {
        std::string normalized = normalizeIdentityToken(owner);
        if (normalized.empty()) continue;
        if (!email.empty() && normalized == email) return true;
        if (!name.empty() && normalized == name) return true;
        if (!login.empty() && normalized == login) return true;
    }
    return false;
}

static bool currentUserOwnsGitHubRemote() {
    GitHubRepoSlug slug = inferGitHubRepoFromOrigin();
    if (slug.owner.empty()) return false;
    std::string login = currentGitHubLogin();
    std::string name = currentGitUserName();
    if (!login.empty() && hasGitHubMaintainerAccess(slug, login)) return true;
    return !name.empty() && normalizeIdentityToken(name) == normalizeIdentityToken(slug.owner);
}

static std::string normalizeCodeOwner(const std::string& owner) {
    if (owner.empty()) return "";
    if (owner[0] == '@') return owner;
    return "@" + owner;
}

static std::string defaultCodeOwners(const std::string& githubOwner) {
    std::string owner = githubOwner.empty() ? "@OWNER_OR_TEAM" : normalizeCodeOwner(githubOwner);
    std::ostringstream out;
    out << "# Ghost governance files\n";
    out << "# Replace @OWNER_OR_TEAM if this was generated from a placeholder.\n";
    out << "/ghost.yml " << owner << "\n";
    out << "/ghost-policy.sig " << owner << "\n";
    out << "/.github/workflows/ghost-audit.yml " << owner << "\n";
    out << "/GHOST.md " << owner << "\n";
    return out.str();
}

static std::string defaultGhostAuditWorkflow() {
    return R"GHOSTYAML(name: Ghost Audit

on:
  pull_request:
    types: [opened, synchronize, reopened]

permissions:
  contents: read
  pull-requests: write

jobs:
  audit:
    runs-on: ubuntu-latest
    steps:
      - name: Checkout PR
        uses: actions/checkout@v4
        with:
          fetch-depth: 0

      - name: Fetch Ghost notes
        run: |
          git fetch origin refs/notes/ghost:refs/notes/ghost 2>/dev/null || true
          git fetch origin refs/notes/ghost-verified:refs/notes/ghost-verified 2>/dev/null || true
          git fetch origin refs/notes/ghost-signatures:refs/notes/ghost-signatures 2>/dev/null || true
          git fetch origin refs/notes/ai:refs/notes/ai 2>/dev/null || true

      - name: Inspect PR governance changes
        id: governance
        run: |
          BASE_SHA="${{ github.event.pull_request.base.sha }}"
          HEAD_SHA="${{ github.event.pull_request.head.sha }}"

          git diff --name-only "${BASE_SHA}" "${HEAD_SHA}" > changed_files.txt
          git diff --name-status "${BASE_SHA}" "${HEAD_SHA}" > changed_files_status.txt

          POLICY_CHANGED="false"
          WORKFLOW_CHANGED="false"
          WORKFLOW_REMOVED="false"

          if grep -qx 'ghost.yml' changed_files.txt; then
            POLICY_CHANGED="true"
          fi

          if grep -qx '.github/workflows/ghost-audit.yml' changed_files.txt; then
            WORKFLOW_CHANGED="true"
          fi

          if grep -q '^D[[:space:]]*.github/workflows/ghost-audit.yml$' changed_files_status.txt; then
            WORKFLOW_REMOVED="true"
          fi

          echo "policy_changed=${POLICY_CHANGED}" >> "$GITHUB_OUTPUT"
          echo "workflow_changed=${WORKFLOW_CHANGED}" >> "$GITHUB_OUTPUT"
          echo "workflow_removed=${WORKFLOW_REMOVED}" >> "$GITHUB_OUTPUT"

      - name: Install Ghost
        run: curl -sSL https://raw.githubusercontent.com/farhankhan197/ghost/main/install.sh | bash

      - name: Run Ghost final-diff audit
        id: audit
        run: |
          ghost verify-pr \
            ${{ github.event.pull_request.base.sha }}..${{ github.event.pull_request.head.sha }} \
            --base origin/${{ github.event.pull_request.base.ref }} \
            --json > audit.json || true

          BLOCKED=$(node -e "const j=JSON.parse(require('fs').readFileSync('audit.json','utf8')); console.log(j.blocked ? 'true' : 'false')")
          echo "blocked=${BLOCKED}" >> "$GITHUB_OUTPUT"
          cat audit.json

      - name: Verify Ghost signatures
        run: |
          if [ -f ghost-policy.sig ]; then
            ghost policy verify --trusted
          else
            echo "No ghost-policy.sig found; skipping policy signature verification."
          fi

          if git notes --ref=refs/notes/ghost-signatures list 2>/dev/null | grep -q .; then
            ghost notes verify --range ${{ github.event.pull_request.base.sha }}..${{ github.event.pull_request.head.sha }} --trusted
          else
            echo "No Ghost note signatures found; skipping note signature verification."
          fi

      - name: Comment on PR
        uses: actions/github-script@v7
        env:
          CONFIG_REF: origin/${{ github.event.pull_request.base.ref }}
          POLICY_CHANGED: ${{ steps.governance.outputs.policy_changed }}
          WORKFLOW_CHANGED: ${{ steps.governance.outputs.workflow_changed }}
          WORKFLOW_REMOVED: ${{ steps.governance.outputs.workflow_removed }}
        with:
          github-token: ${{ secrets.GITHUB_TOKEN }}
          script: |
            const fs = require('fs');
            const audit = JSON.parse(fs.readFileSync('audit.json', 'utf8'));
            const configRef = process.env.CONFIG_REF || 'origin/main';
            const policyChanged = process.env.POLICY_CHANGED === 'true';
            const workflowChanged = process.env.WORKFLOW_CHANGED === 'true';
            const workflowRemoved = process.env.WORKFLOW_REMOVED === 'true';

            const pct = audit.total_lines > 0 ? Math.round((audit.ai_lines / audit.total_lines) * 100) : 0;
            let body = '## Ghost Audit Report\n\n';
            body += `**Status:** ${audit.blocked ? 'BLOCKED' : audit.passed ? 'PASSED' : 'WARNING'}\n\n`;
            body += `**AI Density:** ${pct}% (${audit.ai_lines} / ${audit.total_lines} lines)\n\n`;
            body += `**Policy source:** \`${configRef}:ghost.yml\`\n\n`;
            body += '> Ghost audits this PR with the base-branch policy, not policy changes made inside this PR.\n\n';
            if (audit.message) body += `> ${audit.message}\n\n`;

            if (policyChanged || workflowChanged) {
              body += '### Governance Changes\n\n';
              if (policyChanged) body += '- `ghost.yml` changed in this PR. Policy changes only apply after maintainers merge them.\n';
              if (workflowChanged && !workflowRemoved) body += '- `.github/workflows/ghost-audit.yml` changed in this PR. Maintainers should review enforcement changes carefully.\n';
              if (workflowRemoved) body += '- `.github/workflows/ghost-audit.yml` was removed in this PR. Keep Ghost Audit required before merging policy-sensitive changes.\n';
              body += '\n';
            }

            if (audit.blocked) {
              body += '### Fix Locally\n\n';
              body += '```bash\n';
              body += 'ghost init --contributor\n';
              body += 'ghost status\n';
              body += `ghost verify-pr origin/${{ github.event.pull_request.base.ref }}..HEAD --base origin/${{ github.event.pull_request.base.ref }}\n`;
              body += '```\n';
            }

            const { data: comments } = await github.rest.issues.listComments({
              owner: context.repo.owner,
              repo: context.repo.repo,
              issue_number: context.issue.number
            });
            const existing = comments.find(c => c.body.includes('Ghost Audit Report'));
            if (existing) {
              await github.rest.issues.updateComment({
                owner: context.repo.owner,
                repo: context.repo.repo,
                comment_id: existing.id,
                body
              });
            } else {
              await github.rest.issues.createComment({
                owner: context.repo.owner,
                repo: context.repo.repo,
                issue_number: context.issue.number,
                body
              });
            }

      - name: Fail if owner policy blocks
        if: steps.audit.outputs.blocked == 'true'
        run: |
          echo "Ghost audit blocked this PR because repo owner policy failed."
          exit 1
)GHOSTYAML";
}

static std::string defaultGhostContributorGuide(const ghost::config::GhostConfig& cfg) {
    std::ostringstream out;
    out << "# AI Attribution Policy\n\n";
    out << "This repository uses Ghost to track AI-authored code.\n\n";
    out << "## Maintainer Policy\n\n";
    out << "- Ghost required: " << (cfg.required ? "true" : "false") << "\n";
    out << "- AI threshold: " << cfg.threshold << "%\n";
    out << "- Unverified commits: " << cfg.unverified_policy << "\n";
    if (!cfg.owner.empty()) out << "- Policy owner: " << cfg.owner << "\n";
    out << "\n## Contributor Setup\n\n";
    out << "Run:\n\n";
    out << "```bash\n";
    out << "ghost init --contributor\n";
    out << "```\n\n";
    out << "Before committing:\n\n";
    out << "```bash\n";
    out << "ghost status\n";
    out << "git add <files>\n";
    out << "ghost check\n";
    out << "```\n\n";
    out << "Before pushing:\n\n";
    out << "```bash\n";
    out << "ghost verify-pr origin/main..HEAD\n";
    out << "```\n\n";
    out << "Pull requests are audited by the Ghost Audit required check.\n";
    return out.str();
}

static bool writeOwnerArtifacts(const std::string& repoRoot, const ghost::config::GhostConfig& cfg, bool force, const std::string& githubOwner) {
    bool ok = true;
    ok = writeTextFileIfMissing(
        std::filesystem::path(repoRoot) / ".github" / "workflows" / "ghost-audit.yml",
        defaultGhostAuditWorkflow(),
        force
    ) && ok;
    ok = writeTextFileIfMissing(
        std::filesystem::path(repoRoot) / "GHOST.md",
        defaultGhostContributorGuide(cfg),
        force
    ) && ok;
    ok = writeTextFileIfMissing(
        std::filesystem::path(repoRoot) / ".github" / "CODEOWNERS",
        defaultCodeOwners(githubOwner),
        force
    ) && ok;
    return ok;
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
    logVerbose("showing Ghost note for: " + commit_sha);
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
        std::cout << ghost::output::Style::warning("  No Ghost note found for " + commit_sha) << "\n";
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

    if (!configRef.empty() && !ghost::git::Ref::isSafeConfigRef(configRef)) {
        std::cerr << ghost::output::Style::error("Invalid config ref") << "\n";
        return GHOST_EXIT_ERROR;
    }
    if (!range.empty() && !ghost::git::Ref::isSafeRange(range)) {
        std::cerr << ghost::output::Style::error("Invalid commit range") << "\n";
        return GHOST_EXIT_ERROR;
    }

    logVerbose("audit mode: " + std::string(allMode ? "all" : (range.empty() ? "head" : "range")));
    if (!configRef.empty()) logVerbose("config ref: " + configRef);
    
    if (allMode || !range.empty()) {
        ghost::output::AnimatedSpinner spinner("scanning commits", !jsonOutput);
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
        if (!ghost::git::Ref::isSafeCommitish(target)) {
            std::cerr << ghost::output::Style::error("Invalid commit reference") << "\n";
            return GHOST_EXIT_ERROR;
        }
        logVerbose("single commit audit: " + target);
        ghost::output::AnimatedSpinner spinner("scanning codebase", !jsonOutput);
        auto cbReport = ghost::audit::Auditor::runCodebaseBlame(repoRoot, target, threshold, jsonOutput, configRef);
        spinner.stop();
        if (jsonOutput) {
            std::cout << ghost::output::Report::formatCodebaseJSON(cbReport.summary, cbReport.policy);
        } else {
            ghost::output::Report::streamCodebaseCLI(cbReport.summary, cbReport.policy);
        }
        return cbReport.policy.blocked ? GHOST_EXIT_BLOCKED : GHOST_EXIT_OK;
    } else {
        ghost::output::AnimatedSpinner spinner("scanning codebase", !jsonOutput);
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

static int handleVerifyPr(int argc, char* argv[]) {
    std::string repoRoot = ghost::git::Repo::getRoot();
    if (repoRoot.empty()) {
        std::cerr << ghost::output::Style::error("Not in a git repository") << "\n";
        return GHOST_EXIT_NOT_IN_REPO;
    }

    std::string base = getArg(argc, argv, "--base");
    if (base.empty()) base = "origin/main";
    if (!ghost::git::Ref::isSafeConfigRef(base)) {
        std::cerr << ghost::output::Style::error("Invalid base ref") << "\n";
        return GHOST_EXIT_ERROR;
    }

    std::string range;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--json" || arg == "-j" || arg == "--base") {
            if (arg == "--base" && i + 1 < argc) i++;
            continue;
        }
        if (!arg.empty() && arg[0] != '-') {
            range = arg;
            break;
        }
    }
    if (range.empty()) {
        range = base + "..HEAD";
    }
    if (!ghost::git::Ref::isSafeRange(range)) {
        std::cerr << ghost::output::Style::error("Invalid commit range") << "\n";
        return GHOST_EXIT_ERROR;
    }

    bool jsonOutput = hasFlag(argc, argv, "--json") || hasFlag(argc, argv, "-j");
    bool noFetch = hasFlag(argc, argv, "--no-fetch");
    std::string configRef = base;

    logVerbose("verify-pr range=" + range + " config-ref=" + configRef);

    std::string remote = "origin";
    size_t slash = base.find('/');
    if (slash != std::string::npos && slash > 0) {
        remote = base.substr(0, slash);
    }
    if (!ghost::git::Ref::isSafeToken(remote)) {
        std::cerr << ghost::output::Style::error("Invalid remote name") << "\n";
        return GHOST_EXIT_ERROR;
    }

    bool attemptedFetch = false;
    bool fetchAvailable = false;
    if (!noFetch) {
        std::string remoteUrl = execCommand("git remote get-url " + remote + " 2>&1");
        fetchAvailable = !remoteUrl.empty() &&
            remoteUrl.find("No such remote") == std::string::npos &&
            remoteUrl.find("not appear to be a git repository") == std::string::npos;
        if (fetchAvailable) {
            attemptedFetch = true;
            execCommand("git fetch " + remote + " refs/notes/ghost:refs/notes/ghost 2>&1");
            execCommand("git fetch " + remote + " refs/notes/ghost-verified:refs/notes/ghost-verified 2>&1");
            execCommand("git fetch " + remote + " refs/notes/ghost-signatures:refs/notes/ghost-signatures 2>&1");
            execCommand("git fetch " + remote + " refs/notes/ai:refs/notes/ai 2>&1");
        }
    }

    auto cfg = ghost::config::GhostConfigReader::loadFromRef(repoRoot, configRef);
    bool finalDiffMode = cfg.enforcement_scope.empty() || cfg.enforcement_scope == "final_diff";

    ghost::output::AnimatedSpinner spinner(finalDiffMode ? "verifying final diff policy" : "verifying PR history policy", !jsonOutput);
    auto report = finalDiffMode
        ? ghost::audit::Auditor::runFinalDiff(repoRoot, range, -1, jsonOutput, configRef)
        : ghost::audit::Auditor::run(repoRoot, range, -1, jsonOutput, configRef);
    if (finalDiffMode && cfg.history_policy != "ignore") {
        auto historyReport = ghost::audit::Auditor::run(repoRoot, range, -1, jsonOutput, configRef);
        if (historyReport.policy.blocked) {
            if (cfg.history_policy == "block") {
                report.policy.passed = false;
                report.policy.blocked = true;
                report.policy.message += "\nHistorical commit audit is configured to block: " + historyReport.policy.message;
            } else {
                report.policy.message += "\nHistory warning: " + historyReport.policy.message;
            }
        }
    }
    spinner.stop();

    if (jsonOutput) {
        std::cout << ghost::output::Report::formatJSON(report.summary, report.policy);
        return report.policy.blocked ? GHOST_EXIT_BLOCKED : GHOST_EXIT_OK;
    }

    using namespace ghost::output;

    std::cout << Style::header("Local PR Verification");
    std::cout << "  " << Style::subHeader("Policy");
    std::cout << "    " << Style::label("source") << "      " << Style::violet(configRef + ":ghost.yml") << "\n";
    std::cout << "    " << Style::label("mode") << "        " << Style::violet(cfg.mode.empty() ? "custom" : cfg.mode) << "\n";
    std::cout << "    " << Style::label("scope") << "       " << Style::violet(finalDiffMode ? "final_diff" : "commit_history") << "\n";
    std::cout << "    " << Style::label("history") << "     " << Style::violet(cfg.history_policy.empty() ? "warn" : cfg.history_policy) << "\n";
    std::cout << "    " << Style::label("threshold") << "   " << Style::violet(std::to_string(cfg.threshold) + "%") << "\n";
    std::cout << "    " << Style::label("unverified") << "  " << Style::violet(cfg.unverified_policy) << "\n\n";

    std::cout << "  " << Style::subHeader("Range");
    std::cout << "    " << Style::violet(range) << "\n\n";

    std::cout << "  " << Style::subHeader("Notes");
    if (noFetch) {
        std::cout << "    " << Style::warning("fetch skipped") << Style::dim("  --no-fetch was set") << "\n\n";
    } else if (attemptedFetch) {
        std::cout << "    " << Style::success("fetched") << Style::dim("  Ghost note refs from " + remote) << "\n\n";
    } else {
        std::cout << "    " << Style::warning("not fetched") << Style::dim("  remote " + remote + " was not available") << "\n\n";
    }

    std::cout << ghost::output::Report::formatCLI(report.summary, report.policy, true);

    if (report.policy.blocked) {
        std::cout << Style::dim("  Fix: run 'ghost init --contributor', recommit affected changes, then push notes.\n\n");
    }

    return report.policy.blocked ? GHOST_EXIT_BLOCKED : GHOST_EXIT_OK;
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
        auto cfg = ghost::config::GhostConfigReader::load(repoRoot);
        if (isProtectedPolicyKey(argv[3]) && !requirePolicyOwner(cfg, "change protected policy")) {
            return GHOST_EXIT_ERROR;
        }
        if (isProtectedPolicyKey(argv[3]) &&
            lowerString(argv[3]) != "locked" &&
            lowerString(argv[3]) != "policy_locked" &&
            !requirePolicyUnlocked(cfg, "change protected policy")) {
            return GHOST_EXIT_ERROR;
        }
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
        std::cout << b("mode") << "       " << w(cfg.mode.empty() ? "custom" : cfg.mode) << "\n";
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

static int handlePolicy(int argc, char* argv[]) {
    std::string repoRoot = ghost::git::Repo::getRoot();
    if (repoRoot.empty()) {
        std::cerr << ghost::output::Style::error("Not in a git repository") << "\n";
        return GHOST_EXIT_NOT_IN_REPO;
    }

    auto cfg = ghost::config::GhostConfigReader::load(repoRoot);

    if (argc >= 3 && std::string(argv[2]) == "lock") {
        if (!requirePolicyOwner(cfg, "lock policy")) {
            return GHOST_EXIT_ERROR;
        }
        if (!ghost::config::GhostConfigReader::save(repoRoot, "locked", "true")) {
            std::cerr << ghost::output::Style::error("Failed to update ghost.yml") << "\n";
            return GHOST_EXIT_ERROR;
        }
        std::cout << ghost::output::Style::success("Policy locked") << "\n";
        std::cout << ghost::output::Style::dim("  Recommended: require CODEOWNERS review for ghost.yml and Ghost workflow changes.\n");
        return GHOST_EXIT_OK;
    }

    if (argc >= 3 && std::string(argv[2]) == "sign") {
        if (!requirePolicyOwner(cfg, "sign policy")) {
            return GHOST_EXIT_ERROR;
        }
        std::string policyPath = (std::filesystem::path(repoRoot) / "ghost.yml").string();
        std::string digest = hashFile(policyPath);
        if (digest.empty()) {
            std::cerr << ghost::output::Style::error("Failed to hash ghost.yml") << "\n";
            return GHOST_EXIT_ERROR;
        }
        std::string signer = ghost::git::Repo::getUserEmail();
        long long ts = static_cast<long long>(std::time(nullptr));
        std::ostringstream sig;
        if (ghost::signing::hasTrustedSigners(cfg)) {
            std::string signerPrincipal = signer.empty() ? "unknown" : signer;
            std::string payload = ghost::signing::canonicalPolicyPayload(repoRoot, digest, signerPrincipal, ts);
            auto signedPayload = ghost::signing::signPayload(repoRoot, "ghost-policy", payload, cfg);
            if (!signedPayload.ok) {
                std::cerr << ghost::output::Style::error("Failed to create trusted SSH policy signature.\n")
                          << ghost::output::Style::dim("  " + signedPayload.error + "\n");
                return GHOST_EXIT_ERROR;
            }
            sig << "schema: ghost-policy-signature/2\n";
            sig << "policy: ghost.yml\n";
            sig << "digest: " << digest << "\n";
            sig << "signer: " << signedPayload.signer << "\n";
            sig << "ts: " << ts << "\n";
            sig << "locked: " << (cfg.policy_locked ? "true" : "false") << "\n";
            sig << "namespace: ghost-policy\n";
            sig << "key_fingerprint: " << signedPayload.key_fingerprint << "\n";
            sig << "payload_b64: " << signedPayload.payload_b64 << "\n";
            sig << "signature_b64: " << signedPayload.signature_b64 << "\n";
            signer = signedPayload.signer;
        } else {
            sig << "schema: ghost-policy-signature/1\n";
            sig << "policy: ghost.yml\n";
            sig << "digest: " << digest << "\n";
            sig << "signer: " << (signer.empty() ? "unknown" : signer) << "\n";
            sig << "ts: " << ts << "\n";
            sig << "locked: " << (cfg.policy_locked ? "true" : "false") << "\n";
        }

        std::ofstream out(std::filesystem::path(repoRoot) / "ghost-policy.sig");
        if (!out.is_open()) {
            std::cerr << ghost::output::Style::error("Failed to write ghost-policy.sig") << "\n";
            return GHOST_EXIT_ERROR;
        }
        out << sig.str();
        std::cout << ghost::output::Style::success("Signed ghost.yml") << "\n";
        std::cout << "  digest: " << digest << "\n";
        std::cout << "  signer: " << (signer.empty() ? "unknown" : signer) << "\n";
        return GHOST_EXIT_OK;
    }

    if (argc >= 3 && std::string(argv[2]) == "verify") {
        std::string sigPath = (std::filesystem::path(repoRoot) / "ghost-policy.sig").string();
        if (!fileExists(sigPath)) {
            std::cerr << ghost::output::Style::error("No ghost-policy.sig found.\n")
                      << ghost::output::Style::dim("  Run 'ghost policy sign' as an owner.\n");
            return GHOST_EXIT_ERROR;
        }
        std::ifstream in(sigPath);
        std::stringstream buffer;
        buffer << in.rdbuf();
        auto sig = parseSimpleSignature(buffer.str());
        bool trustedRequired = hasFlag(argc, argv, "--trusted");
        std::string expected = sig["digest"];
        std::string actual = hashFile((std::filesystem::path(repoRoot) / "ghost.yml").string());
        if (expected.empty() || actual.empty() || expected != actual) {
            std::cerr << ghost::output::Style::error("Policy signature mismatch.\n")
                      << ghost::output::Style::dim("  ghost.yml digest: " + actual + "\n")
                      << ghost::output::Style::dim("  signed digest:    " + (expected.empty() ? "missing" : expected) + "\n");
            return GHOST_EXIT_BLOCKED;
        }
        if (sig["schema"] == "ghost-policy-signature/2") {
            long long ts = parseSignatureTs(sig);
            std::string payload = ghost::signing::canonicalPolicyPayload(repoRoot, actual, sig["signer"], ts);
            if (ghost::signing::base64Decode(sig["payload_b64"]) != payload) {
                std::cerr << ghost::output::Style::error("Policy signature payload mismatch.\n");
                return GHOST_EXIT_BLOCKED;
            }
            std::string verifyError;
            if (!ghost::signing::verifyPayload(repoRoot, "ghost-policy", payload, sig["signature_b64"], sig["signer"], cfg, verifyError)) {
                std::cerr << ghost::output::Style::error("Policy SSH signature verification failed.\n")
                          << ghost::output::Style::dim("  " + verifyError + "\n");
                return GHOST_EXIT_BLOCKED;
            }
        } else if (trustedRequired) {
            std::cerr << ghost::output::Style::error("Trusted policy verification requires a v2 SSH signature.\n")
                      << ghost::output::Style::dim("  Run 'ghost policy sign' with a trusted SSH key.\n");
            return GHOST_EXIT_BLOCKED;
        }
        std::cout << ghost::output::Style::success("Policy signature verified") << "\n";
        std::cout << "  signer: " << (sig["signer"].empty() ? "unknown" : sig["signer"]) << "\n";
        std::cout << "  digest: " << actual << "\n";
        if (sig["schema"] == "ghost-policy-signature/2") {
            std::cout << "  trusted: yes\n";
        }
        return GHOST_EXIT_OK;
    }

    if (argc >= 3 && std::string(argv[2]) == "unlock") {
        if (!hasFlag(argc, argv, "--force")) {
            std::cerr << ghost::output::Style::error("Unlock requires --force.\n")
                      << ghost::output::Style::dim("  Usage: ghost policy unlock --force\n");
            return GHOST_EXIT_ERROR;
        }
        if (!requirePolicyOwner(cfg, "unlock policy")) {
            return GHOST_EXIT_ERROR;
        }
        if (!ghost::config::GhostConfigReader::save(repoRoot, "locked", "false")) {
            std::cerr << ghost::output::Style::error("Failed to update ghost.yml") << "\n";
            return GHOST_EXIT_ERROR;
        }
        std::cout << ghost::output::Style::success("Policy unlocked") << "\n";
        return GHOST_EXIT_OK;
    }

    if (argc >= 5 && std::string(argv[2]) == "set" && std::string(argv[3]) == "mode") {
        if (!requirePolicyOwner(cfg, "change protected policy")) {
            return GHOST_EXIT_ERROR;
        }
        if (!requirePolicyUnlocked(cfg, "change protected policy")) {
            return GHOST_EXIT_ERROR;
        }
        std::string mode = argv[4];
        PolicyModeDefaults defaults;
        if (!getPolicyModeDefaults(mode, defaults)) {
            std::cerr << ghost::output::Style::error("Unknown policy mode: " + mode + "\n")
                      << ghost::output::Style::dim("  Valid modes: permissive, transparent, restrictive, locked\n");
            return GHOST_EXIT_ERROR;
        }
        if (!applyPolicyMode(repoRoot, mode)) {
            std::cerr << ghost::output::Style::error("Failed to update ghost.yml") << "\n";
            return GHOST_EXIT_ERROR;
        }
        std::cout << ghost::output::Style::success("Set policy mode = " + defaults.mode) << "\n";
        std::cout << "  required: " << (defaults.required ? "true" : "false") << "\n";
        std::cout << "  threshold: " << defaults.threshold << "%\n";
        std::cout << "  on_exceed: " << defaults.onExceed << "\n";
        std::cout << "  unverified: " << defaults.unverifiedPolicy << "\n";
        return GHOST_EXIT_OK;
    }

    std::string currentUser;
    bool isOwner = currentUserOwnsPolicy(cfg, currentUser);
    using namespace ghost::output;

    auto yesNo = [&](bool value) {
        return value ? Style::success("yes") : Style::muted("no");
    };
    auto boolValue = [&](bool value) {
        return value ? Style::success("true") : Style::muted("false");
    };

    std::cout << Style::header("Owner Policy");
    std::cout << "  " << Style::dim("Repo-bound AI attribution controls and enforcement.\n\n");

    std::cout << "  " << Style::subHeader("Authority");
    std::cout << "    " << Style::label("owner") << "       "
              << (cfg.owner.empty() ? Style::warning("not set") : Style::violet(cfg.owner)) << "\n";
    if (!cfg.owners.empty()) {
        std::cout << "    " << Style::label("owners") << "      ";
        for (size_t i = 0; i < cfg.owners.size(); ++i) {
            if (i > 0) std::cout << Style::dim(", ");
            std::cout << Style::violet(cfg.owners[i]);
        }
        std::cout << "\n";
    }
    std::cout << "    " << Style::label("git user") << "    "
              << (currentUser.empty() ? Style::warning("unknown") : Style::violet(currentUser)) << "\n";
    std::cout << "    " << Style::label("can edit") << "    "
              << ((cfg.owner.empty() && cfg.owners.empty()) ? Style::warning("yes, until owner is set") : yesNo(isOwner)) << "\n";
    std::cout << "    " << Style::label("locked") << "      " << boolValue(cfg.policy_locked) << "\n\n";

    std::cout << "  " << Style::subHeader("Rules");
    std::cout << "    " << Style::label("mode") << "        " << Style::violet(cfg.mode.empty() ? "custom" : cfg.mode)
              << Style::dim("  policy preset") << "\n";
    std::cout << "    " << Style::label("required") << "    " << boolValue(cfg.required)
              << Style::dim("  require Ghost notes before push") << "\n";
    std::cout << "    " << Style::label("threshold") << "   " << Style::violet(std::to_string(cfg.threshold) + "%")
              << Style::dim("  maximum AI-authored line share") << "\n";
    std::cout << "    " << Style::label("on_exceed") << "   " << Style::violet(cfg.on_exceed)
              << Style::dim("  block, warn, or allow when threshold is exceeded") << "\n";
    std::cout << "    " << Style::label("unverified") << "  " << Style::violet(cfg.unverified_policy)
              << Style::dim("  commits missing ghost-verified notes") << "\n";
    std::cout << "    " << Style::label("gitai_fb") << "    " << boolValue(cfg.gitai_fallback)
              << Style::dim("  read refs/notes/ai when Ghost notes are absent") << "\n\n";

    std::cout << "  " << Style::subHeader("Enforcement");
    std::cout << "    " << Style::label("scope") << "         "
              << Style::violet(cfg.enforcement_scope.empty() ? "final_diff" : cfg.enforcement_scope)
              << Style::dim("  final_diff or commit_history") << "\n";
    std::cout << "    " << Style::label("history") << "       "
              << Style::violet(cfg.history_policy.empty() ? "warn" : cfg.history_policy)
              << Style::dim("  ignore, warn, or block intermediate commits") << "\n";
    std::cout << "    " << Style::label("ghost status") << "  "
              << Style::dim("repo setup, working tree, uncommitted sessions, and HEAD notes") << "\n";
    std::cout << "    " << Style::label("ghost check") << "   "
              << Style::dim("staged diff preview before commit") << "\n";
    std::cout << "    " << Style::label("ghost audit") << "   "
              << Style::dim("committed history audit") << "\n";
    std::cout << "    " << Style::label("ghost verify") << "  "
              << Style::dim("final PR diff policy gate") << "\n";
    std::cout << "    " << Style::label("CI config") << "    "
              << Style::dim("use verify-pr --base origin/main so PRs cannot weaken policy") << "\n\n";

    if (!cfg.ignore.empty()) {
        std::cout << "  " << Style::subHeader("Banished Paths");
        for (const auto& path : cfg.ignore) {
            std::cout << "    " << Style::violet(path) << "\n";
        }
        std::cout << "\n";
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

    if (cfg.owner.empty()) {
        std::cerr << ghost::output::Style::error("No owner configured for this repo.\n")
                  << ghost::output::Style::dim("  Set the owner with: ghost config set owner <email>\n");
        return GHOST_EXIT_ERROR;
    }
    if (!requirePolicyOwner(cfg, "banish files")) {
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

static std::string buildNoteSignature(const std::string& repoRoot, const std::string& commitSha) {
    auto cfg = ghost::config::GhostConfigReader::load(repoRoot);
    std::string ghostNote = ghost::git::Notes::show("refs/notes/ghost", commitSha);
    std::string verifiedNote = ghost::git::Notes::show("refs/notes/ghost-verified", commitSha);
    std::string signer = ghost::git::Repo::getUserEmail();
    std::string ghostDigest = ghostNote.empty() ? "absent" : hashText(repoRoot, ghostNote);
    std::string verifiedDigest = verifiedNote.empty() ? "absent" : hashText(repoRoot, verifiedNote);
    long long ts = static_cast<long long>(std::time(nullptr));

    std::ostringstream sig;
    if (ghost::signing::hasTrustedSigners(cfg)) {
        std::string signerPrincipal = signer.empty() ? "unknown" : signer;
        std::string payload = ghost::signing::canonicalNotePayload(commitSha, ghostDigest, verifiedDigest, signerPrincipal, ts);
        auto signedPayload = ghost::signing::signPayload(repoRoot, "ghost-notes", payload, cfg);
        if (signedPayload.ok) {
            sig << "schema: ghost-note-signature/2\n";
            sig << "commit: " << commitSha << "\n";
            sig << "ghost_digest: " << ghostDigest << "\n";
            sig << "verified_digest: " << verifiedDigest << "\n";
            sig << "signer: " << signedPayload.signer << "\n";
            sig << "ts: " << ts << "\n";
            sig << "namespace: ghost-notes\n";
            sig << "key_fingerprint: " << signedPayload.key_fingerprint << "\n";
            sig << "payload_b64: " << signedPayload.payload_b64 << "\n";
            sig << "signature_b64: " << signedPayload.signature_b64 << "\n";
            return sig.str();
        }
    }
    sig << "schema: ghost-note-signature/1\n";
    sig << "commit: " << commitSha << "\n";
    sig << "ghost_digest: " << ghostDigest << "\n";
    sig << "verified_digest: " << verifiedDigest << "\n";
    sig << "signer: " << (signer.empty() ? "unknown" : signer) << "\n";
    sig << "ts: " << ts << "\n";
    return sig.str();
}

static int handleNotes(int argc, char* argv[]) {
    std::string repoRoot = ghost::git::Repo::getRoot();
    if (repoRoot.empty()) {
        std::cerr << ghost::output::Style::error("Not in a git repository") << "\n";
        return GHOST_EXIT_NOT_IN_REPO;
    }
    if (argc < 3) {
        ghost::cli::CommandRegistry::printHelp("notes");
        return GHOST_EXIT_ERROR;
    }

    std::string action = argv[2];
    std::string range = getArg(argc, argv, "--range");
    bool trustedRequired = hasFlag(argc, argv, "--trusted");
    std::string commitSha = (argc >= 4 && std::string(argv[3])[0] != '-')
        ? argv[3]
        : ghost::git::Repo::getHead();
    if (commitSha.empty() && range.empty()) {
        std::cerr << ghost::output::Style::error("No commit selected") << "\n";
        return GHOST_EXIT_ERROR;
    }
    if (!commitSha.empty() && !ghost::git::Ref::isSafeCommitish(commitSha)) {
        std::cerr << ghost::output::Style::error("Invalid commit reference") << "\n";
        return GHOST_EXIT_ERROR;
    }
    if (!commitSha.empty()) {
        std::string resolved = execCommand("git rev-parse --verify " + commitSha + " 2>&1");
        if (!resolved.empty() && resolved.find("fatal:") == std::string::npos) {
            commitSha = resolved;
        }
    }

    if (action == "sign") {
        std::string sig = buildNoteSignature(repoRoot, commitSha);
        if (!ghost::git::Notes::write("refs/notes/ghost-signatures", commitSha, sig)) {
            std::cerr << ghost::output::Style::error("Failed to write note signature") << "\n";
            return GHOST_EXIT_ERROR;
        }
        std::cout << ghost::output::Style::success("Signed Ghost notes for " + commitSha.substr(0, 8)) << "\n";
        return GHOST_EXIT_OK;
    }

    if (action == "verify") {
        if (!range.empty()) {
            if (!ghost::git::Ref::isSafeRange(range)) {
                std::cerr << ghost::output::Style::error("Invalid commit range") << "\n";
                return GHOST_EXIT_ERROR;
            }
            std::string commits = execCommand("git rev-list " + range + " 2>&1");
            if (commits.empty()) {
                std::cout << ghost::output::Style::success("No commits to verify in range") << "\n";
                return GHOST_EXIT_OK;
            }
            std::istringstream stream(commits);
            std::string sha;
            bool ok = true;
            while (std::getline(stream, sha)) {
                while (!sha.empty() && (sha.back() == '\n' || sha.back() == '\r')) sha.pop_back();
                if (sha.empty()) continue;
                const char* fakeArgvTrusted[] = {argv[0], "notes", "verify", sha.c_str(), "--trusted"};
                const char* fakeArgv[] = {argv[0], "notes", "verify", sha.c_str()};
                int rc = trustedRequired
                    ? handleNotes(5, const_cast<char**>(fakeArgvTrusted))
                    : handleNotes(4, const_cast<char**>(fakeArgv));
                if (rc != GHOST_EXIT_OK) ok = false;
            }
            return ok ? GHOST_EXIT_OK : GHOST_EXIT_BLOCKED;
        }

        std::string rawSig = ghost::git::Notes::show("refs/notes/ghost-signatures", commitSha);
        if (rawSig.empty()) {
            std::cerr << ghost::output::Style::error("No Ghost note signature found for " + commitSha.substr(0, 8) + "\n")
                      << ghost::output::Style::dim("  Run 'ghost notes sign " + commitSha + "'.\n");
            return GHOST_EXIT_BLOCKED;
        }
        auto sig = parseSimpleSignature(rawSig);
        std::string ghostNote = ghost::git::Notes::show("refs/notes/ghost", commitSha);
        std::string verifiedNote = ghost::git::Notes::show("refs/notes/ghost-verified", commitSha);
        std::string ghostDigest = ghostNote.empty() ? "absent" : hashText(repoRoot, ghostNote);
        std::string verifiedDigest = verifiedNote.empty() ? "absent" : hashText(repoRoot, verifiedNote);

        bool ok = sig["ghost_digest"] == ghostDigest && sig["verified_digest"] == verifiedDigest;
        if (!ok) {
            std::cerr << ghost::output::Style::error("Ghost note signature mismatch for " + commitSha.substr(0, 8) + "\n")
                      << ghost::output::Style::dim("  ghost_digest:    " + ghostDigest + "\n")
                      << ghost::output::Style::dim("  signed ghost:    " + (sig["ghost_digest"].empty() ? "missing" : sig["ghost_digest"]) + "\n")
                      << ghost::output::Style::dim("  verified_digest: " + verifiedDigest + "\n")
                      << ghost::output::Style::dim("  signed verified: " + (sig["verified_digest"].empty() ? "missing" : sig["verified_digest"]) + "\n");
            return GHOST_EXIT_BLOCKED;
        }
        if (sig["schema"] == "ghost-note-signature/2") {
            long long ts = parseSignatureTs(sig);
            std::string payload = ghost::signing::canonicalNotePayload(commitSha, ghostDigest, verifiedDigest, sig["signer"], ts);
            if (ghost::signing::base64Decode(sig["payload_b64"]) != payload) {
                std::cerr << ghost::output::Style::error("Ghost note signature payload mismatch for " + commitSha.substr(0, 8) + "\n");
                return GHOST_EXIT_BLOCKED;
            }
            auto cfg = ghost::config::GhostConfigReader::load(repoRoot);
            std::string verifyError;
            if (!ghost::signing::verifyPayload(repoRoot, "ghost-notes", payload, sig["signature_b64"], sig["signer"], cfg, verifyError)) {
                std::cerr << ghost::output::Style::error("Ghost note SSH signature verification failed for " + commitSha.substr(0, 8) + "\n")
                          << ghost::output::Style::dim("  " + verifyError + "\n");
                return GHOST_EXIT_BLOCKED;
            }
        } else if (trustedRequired) {
            std::cerr << ghost::output::Style::error("Trusted note verification requires a v2 SSH signature for " + commitSha.substr(0, 8) + "\n");
            return GHOST_EXIT_BLOCKED;
        }
        std::cout << ghost::output::Style::success("Ghost note signature verified for " + commitSha.substr(0, 8)) << "\n";
        std::cout << "  signer: " << (sig["signer"].empty() ? "unknown" : sig["signer"]) << "\n";
        if (sig["schema"] == "ghost-note-signature/2") {
            std::cout << "  trusted: yes\n";
        }
        return GHOST_EXIT_OK;
    }

    std::cerr << ghost::output::Style::error("Unknown notes action: " + action + "\n")
              << ghost::output::Style::dim("  Usage: ghost notes sign [commit]\n")
              << ghost::output::Style::dim("         ghost notes verify [commit]\n");
    return GHOST_EXIT_ERROR;
}

static int handleInstallHooks(int argc, char* argv[]) {
    std::string specificAgent;
    for (int i = 2; i < argc - 1; ++i) {
        if (std::string(argv[i]) == "--agent" && i + 1 < argc) {
            specificAgent = argv[i + 1];
            break;
        }
    }

    logVerbose("install global agent hooks, agent=" + specificAgent);
    ghost::hooks::Installer::installBin();
    if (!specificAgent.empty()) {
        if (!ghost::hooks::AgentHooks::installForAgent("", specificAgent, true)) return GHOST_EXIT_ERROR;
    } else {
        if (!ghost::hooks::AgentHooks::installAll("", true)) return GHOST_EXIT_ERROR;
    }
    return GHOST_EXIT_OK;
}

static int handleUninstallHooks(int argc, char* argv[]) {
    std::string specificAgent;
    for (int i = 2; i < argc - 1; ++i) {
        if (std::string(argv[i]) == "--agent" && i + 1 < argc) {
            specificAgent = argv[i + 1];
            break;
        }
    }

    logVerbose("uninstall global agent hooks, agent=" + specificAgent);
    if (!specificAgent.empty()) {
        ghost::hooks::AgentHooks::uninstallForAgent("", specificAgent, true);
    } else {
        ghost::hooks::AgentHooks::uninstallAll("", true);
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
    bool ownerMode = hasFlag(argc, argv, "--owner");
    bool contributorMode = hasFlag(argc, argv, "--contributor");
    bool explicitOwnerMode = ownerMode;
    bool explicitContributorMode = contributorMode;
    bool force = hasFlag(argc, argv, "--force");
    std::string requestedMode = getArg(argc, argv, "--mode");
    std::string githubOwner = getArg(argc, argv, "--github-owner");

    if (ownerMode && contributorMode) {
        std::cerr << ghost::output::Style::error("Choose either --owner or --contributor, not both") << "\n";
        return GHOST_EXIT_ERROR;
    }

    std::string ymlPath = repoRoot + "/ghost.yml";
    bool ymlExists = fileExists(ymlPath);
    GitHubRepoSlug remoteSlug = inferGitHubRepoFromOrigin();
    bool hasGitHubRemote = !remoteSlug.owner.empty();
    bool canOwnExistingPolicy = false;
    if (ymlExists) {
        auto existingCfg = ghost::config::GhostConfigReader::load(repoRoot);
        bool remoteSaysOwner = currentUserOwnsGitHubRemote();
        canOwnExistingPolicy = hasGitHubRemote ? remoteSaysOwner : currentUserMatchesGhostOwner(existingCfg);
    } else {
        canOwnExistingPolicy = !hasGitHubRemote || currentUserOwnsGitHubRemote();
    }

    if (!explicitOwnerMode && !explicitContributorMode) {
        ownerMode = canOwnExistingPolicy;
        contributorMode = !ownerMode;
        std::cout << "  " << ghost::output::Style::dim(
            std::string("Detected repo role: ") + (ownerMode ? "owner" : "contributor")
        ) << "\n";
    }

    if (ownerMode && !canOwnExistingPolicy) {
        std::cerr << ghost::output::Style::error("This repository appears to be owned by someone else.\n")
                  << ghost::output::Style::dim("  Running contributor setup instead preserves owner policy. Use --contributor or ask a maintainer for access.\n");
        return GHOST_EXIT_ERROR;
    }

    if (requestedMode.empty() && ownerMode && (!ymlExists || explicitOwnerMode)) {
        requestedMode = "restrictive";
    }
    if (githubOwner.empty() && ownerMode) {
        githubOwner = inferGitHubOwnerFromOrigin();
    }

    logVerbose("init repo=" + repoRoot + " yes=" + std::to_string(yesMode) +
               " interactive=" + std::to_string(interactive) + " dryRun=" + std::to_string(dryRun) +
               " owner=" + std::to_string(ownerMode) + " contributor=" + std::to_string(contributorMode));

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
    std::string policyMode = requestedMode.empty() ? "custom" : lowerString(requestedMode);
    std::vector<std::string> ignorePatterns;
    std::vector<std::string> selectedAgents;

    if (!requestedMode.empty()) {
        PolicyModeDefaults defaults;
        if (!getPolicyModeDefaults(requestedMode, defaults)) {
            std::cerr << Style::error("Unknown policy mode: " + requestedMode + "\n")
                      << Style::dim("  Valid modes: permissive, transparent, restrictive, locked\n");
            return GHOST_EXIT_ERROR;
        }
        required = defaults.required;
        threshold = defaults.threshold;
        onExceed = defaults.onExceed;
        unverifiedPolicy = defaults.unverifiedPolicy;
        policyMode = defaults.mode;
    }

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
        std::cout << Style::dim("  Configure Ghost for this repository.\n\n");

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
        int reqChoice = selectMenu("Require Ghost attribution?", reqOpts, 0);
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

    if (selectedAgents.empty()) {
        selectedAgents = {"opencode", "codex", "claude", "cursor", "antigravity"};
    }

    // Dry run preview
    if (dryRun) {
        std::cout << Style::header("Dry Run — ghost init");
        std::cout << "Would configure:\n";
        if (contributorMode) {
            std::cout << "  - local hooks and notes refs only (preserve ghost.yml)\n";
        } else {
            std::cout << "  - ghost.yml (mode=" << policyMode << ", threshold=" << threshold
                      << ", required=" << (required ? "true" : "false") << ")\n";
        }
        std::cout << "  - post-commit hook\n";
        std::cout << "  - pre-push hook\n";
        std::cout << "  - git notes push refs\n";
        std::cout << "  - Ghost binaries in ~/.ghost/bin\n";
        std::cout << "  - global AI agent capture hooks\n";
        if (ownerMode) {
            std::cout << "  - .github/workflows/ghost-audit.yml if missing\n";
            std::cout << "  - GHOST.md if missing\n";
            std::cout << "  - .github/CODEOWNERS if missing";
            if (!githubOwner.empty()) std::cout << " (" << normalizeCodeOwner(githubOwner) << ")";
            std::cout << "\n";
        }
        if (!selectedAgents.empty()) {
            std::cout << "  - agent hooks for: ";
            for (size_t i = 0; i < selectedAgents.size(); ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << selectedAgents[i];
            }
            std::cout << "\n";
        }
        std::cout << "\n";
        return GHOST_EXIT_OK;
    }

    // Write ghost.yml unless this is contributor-only setup.
    if (!contributorMode) {
        bool explicitPolicyChange = explicitOwnerMode || !requestedMode.empty() || interactive || force;
        if (ymlExists && !force && !explicitPolicyChange) {
            std::cout << "  " << Style::dim("ghost.yml already exists; preserving it") << "\n";
        } else if (ymlExists && !force) {
            std::string ownerEmail = ghost::git::Repo::getUserEmail();
            bool ok = true;
            ok = ghost::config::GhostConfigReader::save(repoRoot, "mode", policyMode) && ok;
            ok = ghost::config::GhostConfigReader::save(repoRoot, "threshold", std::to_string(threshold)) && ok;
            ok = ghost::config::GhostConfigReader::save(repoRoot, "required", required ? "true" : "false") && ok;
            ok = ghost::config::GhostConfigReader::save(repoRoot, "on_exceed", onExceed) && ok;
            ok = ghost::config::GhostConfigReader::save(repoRoot, "pr_comment", prComment ? "true" : "false") && ok;
            ok = ghost::config::GhostConfigReader::save(repoRoot, "untagged", untaggedPolicy) && ok;
            ok = ghost::config::GhostConfigReader::save(repoRoot, "unverified", unverifiedPolicy) && ok;
            ok = ghost::config::GhostConfigReader::save(repoRoot, "gitai_fb", gitaiFallback ? "true" : "false") && ok;
            if (!ownerEmail.empty()) {
                ok = ghost::config::GhostConfigReader::save(repoRoot, "owner", ownerEmail) && ok;
            }
            if (!ok) {
                std::cerr << Style::error("Failed to update ghost.yml") << "\n";
                return GHOST_EXIT_ERROR;
            }
            std::cout << "  " << Style::success("Updated ghost.yml policy keys") << "\n";
        } else {
        std::ofstream yml(ymlPath);
        if (!yml) {
            std::cerr << Style::error("Failed to write ghost.yml") << "\n";
            return GHOST_EXIT_ERROR;
        }
        yml << "# Ghost configuration\n";
        yml << "# See: https://github.com/farhankhan197/ghost#configuration\n";
        yml << "\n";
        yml << "version: 1\n";
        yml << "mode: " << policyMode << "\n";
        yml << "locked: false\n";
        yml << "threshold: " << threshold << "\n";
        yml << "required: " << (required ? "true" : "false") << "\n";
        yml << "on_exceed: " << onExceed << "\n";
        yml << "pr_comment: " << (prComment ? "true" : "false") << "\n";
        yml << "untagged: " << untaggedPolicy << "\n";
        yml << "unverified: " << unverifiedPolicy << "\n";
        yml << "gitai_fb: " << (gitaiFallback ? "true" : "false") << "\n";
        yml << "enforcement:\n";
        yml << "  scope: final_diff\n";
        yml << "  history: warn\n";
        std::string ownerEmail = ghost::git::Repo::getUserEmail();
        if (!ownerEmail.empty()) {
            yml << "owner: " << ownerEmail << "\n";
            yml << "owners:\n";
            yml << "  - " << ownerEmail << "\n";
        }
        if (!ignorePatterns.empty()) {
            yml << "ignore:\n";
            for (const auto& p : ignorePatterns) {
                yml << "  - " << p << "\n";
            }
        }
        std::cout << "  " << Style::success(std::string(ymlExists ? "Updated" : "Created") + " ghost.yml") << "\n";
        logVerbose("wrote ghost.yml to " + ymlPath);
        }
    } else if (!ymlExists) {
        std::cerr << Style::error("Contributor setup requires ghost.yml in the repo.\n")
                  << Style::dim("  Ask a maintainer to run: ghost init --owner\n");
        return GHOST_EXIT_ERROR;
    } else {
        std::cout << "  " << Style::success("Found owner policy ghost.yml") << "\n";
    }

    // Ensure both ghost and ghost-checkpoint are available from a stable location.
    int binResult = ghost::hooks::Installer::installBin();
    if (binResult != GHOST_EXIT_OK) {
        std::cerr << Style::warning("Warning: Ghost binaries could not be installed to ~/.ghost/bin") << "\n";
    }

    // Install repo hooks.
    int hooksResult = ghost::hooks::Installer::installRepo(repoRoot);
    if (hooksResult != GHOST_EXIT_OK) {
        std::cerr << Style::warning("Warning: some hooks may not have installed correctly") << "\n";
    }

    configureNotesRefs();

    if (ownerMode) {
        auto cfg = ghost::config::GhostConfigReader::load(repoRoot);
        if (writeOwnerArtifacts(repoRoot, cfg, force, githubOwner)) {
            std::cout << "  " << Style::success("Created owner workflow/docs/CODEOWNERS where missing") << "\n";
        } else {
            std::cerr << Style::warning("  Could not write one or more owner artifacts") << "\n";
        }
    }

    // Install agent hooks
    if (!selectedAgents.empty()) {
        for (const auto& agent : selectedAgents) {
            if (ghost::hooks::AgentHooks::installForAgent(repoRoot, agent, true)) {
                std::cout << "  " << Style::success("Installed global hook for " + agent) << "\n";
            } else {
                std::cerr << Style::warning("  Could not install global hook for " + agent) << "\n";
            }
        }
    }

    std::cout << "\n" << Style::success("Done. Ghost is initialized in this repo.") << "\n";
    if (ownerMode) {
        std::cout << Style::dim("  Next: commit ghost.yml, .github/workflows/ghost-audit.yml, .github/CODEOWNERS, and GHOST.md.\n");
        std::cout << Style::dim("  Then require the \"Ghost Audit\" check and CODEOWNERS review in branch protection.\n");
    } else if (contributorMode) {
        std::cout << Style::dim("  Next: run 'ghost status', then 'ghost check' after staging changes.\n");
        std::cout << Style::dim("  Before pushing: ghost verify-pr origin/main..HEAD\n");
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
            std::cout << "  " << Style::warning("⚠ Ghost not in PATH") << "\n";
            std::cout << "    " << Style::dim("Run 'ghost init' or add ~/.ghost/bin to PATH") << "\n";
            allOk = false;
        } else {
            std::cout << "  " << Style::success("✓ Ghost in PATH") << " " << Style::dim(ghostPath) << "\n";
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

    // Check 6: Global agent hooks
    auto detected = ghost::hooks::AgentDetector::detectInstalled();
    if (detected.empty()) {
        std::cout << "  " << Style::dim("  No AI agents detected") << "\n";
    } else {
        for (const auto& a : detected) {
            std::string agentDir = ghost::hooks::AgentDetector::getGlobalConfigDir(a);
            bool hasHook = !agentDir.empty() && fileExists(agentDir);
            if (hasHook) {
                std::cout << "  " << Style::success("✓ " + a + " global hook") << "\n";
            } else {
                std::cout << "  " << Style::warning("⚠ " + a + " detected but global hook not installed") << "\n";
                if (autoFix) {
                    ghost::hooks::Installer::installBin();
                    if (ghost::hooks::AgentHooks::installForAgent(repoRoot, a, true)) {
                        std::cout << "    " << Style::success("Fixed: installed " + a + " global hook") << "\n";
                    }
                } else {
                    allOk = false;
                }
            }
        }
    }

    std::cout << "\n";
    if (allOk) {
        std::cout << "  " << Style::success("All spirits aligned! 👻") << "\n\n";
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

#ifdef _WIN32
    const std::string quiet = " 2>nul";
#else
    const std::string quiet = " 2>/dev/null";
#endif
    std::string branch = execCommand("git branch --show-current" + quiet);
    if (branch.empty()) branch = "detached";

    std::cout << Style::header("status");
    std::cout << "  " << Style::padRight(Style::dim("repo"), 14) << Style::glow(repoRoot) << "\n";
    std::cout << "  " << Style::padRight(Style::dim("branch"), 14) << Style::violet(branch) << "\n\n";

    // Config
    auto cfg = ghost::config::GhostConfigReader::load(repoRoot);
    std::cout << Style::subHeader("Policy");
    std::cout << "  " << Style::padRight(Style::dim("mode"), 14) << Style::glow(cfg.mode.empty() ? "custom" : cfg.mode) << "\n";
    std::cout << "  " << Style::padRight(Style::dim("required"), 14) << (cfg.required ? Style::success("yes") : Style::dim("no")) << "\n";
    std::cout << "  " << Style::padRight(Style::dim("threshold"), 14) << Style::glow(std::to_string(cfg.threshold) + "%") << "\n";
    std::cout << "  " << Style::padRight(Style::dim("action"), 14) << Style::glow(cfg.on_exceed) << "\n";
    if (!cfg.ignore.empty()) {
        std::cout << "  " << Style::padRight(Style::dim("ignored"), 14) << Style::dim(cfg.ignore[0]);
        for (size_t i = 1; i < cfg.ignore.size(); ++i) {
            std::cout << Style::dim(", " + cfg.ignore[i]);
        }
        std::cout << "\n";
    }

    bool postCommit = fileExists(repoRoot + "/.git/hooks/post-commit");
    bool prePush = fileExists(repoRoot + "/.git/hooks/pre-push");
    std::string notesPush = execCommand("git config --get-all remote.origin.push" + quiet);
    bool notesConfigured = notesPush.find("refs/notes/ghost") != std::string::npos;
    bool localReady = postCommit && prePush && notesConfigured;

    std::cout << "\n" << Style::subHeader("Setup");
    std::cout << "  " << Style::padRight(Style::dim("local"), 14)
              << (localReady ? Style::success("ready") : Style::warning("needs setup"))
              << Style::dim(localReady ? "  capture and push checks are configured" : "  run ghost init --contributor")
              << "\n";
    std::cout << "  " << Style::padRight(Style::dim("notes"), 14)
              << (notesConfigured ? Style::success("configured") : Style::warning("not configured"))
              << Style::dim("  used by audit and PR checks")
              << "\n";

    auto stagedFiles = ghost::git::Diff::getChangedFiles("--cached");
    auto unstagedFiles = ghost::git::Diff::getChangedFiles("");
    std::cout << "\n" << Style::subHeader("Working Tree");
    std::cout << "  " << Style::padRight(Style::dim("staged"), 14) << Style::glow(std::to_string(stagedFiles.size()) + " files")
              << Style::dim("  checked by ghost check") << "\n";
    std::cout << "  " << Style::padRight(Style::dim("unstaged"), 14) << Style::glow(std::to_string(unstagedFiles.size()) + " files")
              << Style::dim("  stage before checking") << "\n";

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

    std::cout << "\n" << Style::subHeader("Pending Attribution");
    std::cout << "  " << Style::dim("Captured AI edits that will attach to the next commit.") << "\n";

    auto* db = ghost::persist::getRepoDb(repoRoot);
    {
        std::vector<ghost::persist::Session> sessions;
        if (db) {
            sessions = db->loadSessions(true);
        }
        normalizePendingSessions(sessions, repoRoot);

        if (sessions.empty()) {
            std::cout << "  " << Style::padRight(Style::dim("state"), 14) << Style::dim("none captured") << "\n";
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

            std::cout << "  " << Style::padRight(Style::dim("sessions"), 14) << Style::glow(std::to_string(sessions.size())) << "\n";
            std::cout << "  " << Style::padRight(Style::dim("captured"), 14)
                      << Style::success("+" + std::to_string(totalAiAdditions))
                      << Style::dim(" / ")
                      << Style::warning("-" + std::to_string(totalAiDeletions)) << "\n";

            // Show cumulative bar (AI-only since ghost doesn't track human edits)
            if (totalAiAdditions > 0 || totalAiDeletions > 0) {
                int total = totalAiAdditions + totalAiDeletions;
                int barWidth = 24;
                int aiChars = (total > 0) ? (totalAiAdditions * barWidth) / total : 0;
                std::string bar;
                for (int i = 0; i < barWidth; ++i) {
                    bar += (i < aiChars) ? "█" : "·";
                }
                int pct = (total > 0) ? std::min((totalAiAdditions * 100) / total, 100) : 0;
                std::cout << "  " << Style::padRight(Style::dim("mix"), 14)
                          << Style::violet(bar)
                          << "  " << Style::glow(std::to_string(pct) + "% additions") << "\n";
            }

            std::cout << "\n";
            for (const auto& s : sessions) {
                std::string agentModel = s.agent + "/" + s.model;
                auto files = extractSessionFiles(s.json_data, repoRoot);
                std::cout << "  " << Style::padRight(Style::dim(timeAgo(s.ts_start)), 14)
                          << Style::padRight(Style::success("+" + std::to_string(s.additions)) + Style::dim("/") + Style::warning("-" + std::to_string(s.deletions)), 12)
                          << Style::padRight(Style::glow(agentModel), 34)
                          << Style::dim(std::to_string(files.size()) + " file" + (files.size() == 1 ? "" : "s")) << "\n";
                for (size_t i = 0; i < std::min<size_t>(files.size(), 3); ++i) {
                    std::cout << "    " << Style::dim(files[i]) << "\n";
                }
                if (files.size() > 3) {
                    std::cout << "    " << Style::dim("+" + std::to_string(files.size() - 3) + " more") << "\n";
                }
            }

        }
    }

    // Notes summary
    std::string headSha = ghost::git::Repo::getHead();
    std::cout << "\n" << Style::subHeader("HEAD Attribution");
    if (headSha.empty()) {
        std::cout << "  " << Style::padRight(Style::dim("state"), 14) << Style::dim("no commits yet") << "\n\n";
        return GHOST_EXIT_OK;
    }
    std::string note = ghost::git::Notes::show("refs/notes/ghost", headSha);
    std::cout << "  " << Style::padRight(Style::dim("commit"), 14) << Style::violet(headSha.substr(0, 8)) << "\n";
    if (!note.empty()) {
        auto parsed = ghost::note::NoteReader::parse(note);
        if (parsed.success) {
            std::set<std::string> files;
            int aiLines = 0;
            for (const auto& e : parsed.entries) {
                files.insert(e.file_path);
                aiLines += static_cast<int>(e.ranges.lineCount());
            }
            std::cout << "  " << Style::padRight(Style::dim("state"), 14) << Style::success("attributed") << "\n";
            std::cout << "  " << Style::padRight(Style::dim("ai lines"), 14) << Style::glow(std::to_string(aiLines)) << "\n";
            std::cout << "  " << Style::padRight(Style::dim("files"), 14) << Style::glow(std::to_string(files.size())) << "\n";
        } else {
            std::cout << "  " << Style::padRight(Style::dim("state"), 14) << Style::warning("unreadable attribution") << "\n";
        }
    } else {
        std::cout << "  " << Style::padRight(Style::dim("state"), 14) << Style::dim("no attribution") << "\n";
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
    if (!configRef.empty() && !ghost::git::Ref::isSafeConfigRef(configRef)) {
        std::cerr << ghost::output::Style::error("Invalid config ref") << "\n";
        return GHOST_EXIT_ERROR;
    }

    using namespace ghost::output;

    auto cfg = configRef.empty()
        ? ghost::config::GhostConfigReader::load(repoRoot)
        : ghost::config::GhostConfigReader::loadFromRef(repoRoot, configRef);

    // Get staged diff stats
    auto stagedFiles = ghost::git::Diff::getChangedFiles("--cached");
    auto stagedRanges = ghost::git::Diff::getChangedRanges(repoRoot, "--cached");
    size_t ignoredStagedFiles = 0;
    stagedFiles.erase(
        std::remove_if(stagedFiles.begin(), stagedFiles.end(),
            [&](const auto& file) {
                bool ignored = shouldIgnorePath(file.path, cfg.ignore);
                if (ignored) ignoredStagedFiles++;
                return ignored;
            }),
        stagedFiles.end()
    );
    if (stagedFiles.empty()) {
        if (jsonOutput) {
            std::cout << "{\n";
            std::cout << "  \"scope\": \"staged_changes\",\n";
            std::cout << "  \"staged_files\": 0,\n";
            std::cout << "  \"ignored_staged_files\": " << ignoredStagedFiles << ",\n";
            std::cout << "  \"total_additions\": 0,\n";
            std::cout << "  \"predicted_ai_additions\": 0,\n";
            std::cout << "  \"predicted_ai_percent\": 0,\n";
            std::cout << "  \"would_pass\": true,\n";
            std::cout << "  \"status\": \"" << (ignoredStagedFiles > 0 ? "ONLY_IGNORED_STAGED_CHANGES" : "NO_STAGED_CHANGES") << "\",\n";
            std::cout << "  \"message\": \"" << (ignoredStagedFiles > 0 ? "All staged changes are ignored by ghost.yml." : "No staged changes to check.") << "\"\n";
            std::cout << "}\n";
            return GHOST_EXIT_OK;
        }
        std::cout << Style::header("check");
        std::cout << "  " << Style::dim("Staged changes only. Stage files before checking attribution.") << "\n\n";
        if (ignoredStagedFiles > 0) {
            std::cout << "  " << Style::warning("Only ignored staged changes") << "\n";
            std::cout << "  " << Style::dim(std::to_string(ignoredStagedFiles) + " staged file(s) matched ghost.yml ignore patterns.") << "\n";
        } else {
            std::cout << "  " << Style::warning("No staged changes to check") << "\n";
            std::cout << "  " << Style::dim("Run 'git add <files>' first, then 'ghost check'.") << "\n";
        }
        std::cout << "  " << Style::dim("Use 'ghost status' to see pending captured attribution.") << "\n\n";
        return GHOST_EXIT_OK;
    }

    std::vector<ghost::persist::Session> uncommittedSessions;
    auto* db = ghost::persist::getRepoDb(repoRoot);
    if (db) {
        uncommittedSessions = db->loadSessions(true);
    }
    normalizePendingSessions(uncommittedSessions, repoRoot);
    std::sort(uncommittedSessions.begin(), uncommittedSessions.end(),
        [](const auto& a, const auto& b) { return a.ts_start > b.ts_start; });

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

        // Determine if this file is likely AI-authored based on uncommitted sessions
        // or existing attribution on HEAD.
        ghost::note::LineRangeSet sessionAttributedRanges;
        const ghost::persist::Session* firstMatchingSession = nullptr;
        auto stagedRangeIt = stagedRanges.added.find(normalizeRepoPath(df.path, repoRoot));
        for (const auto& session : uncommittedSessions) {
            auto ranges = extractSessionRangesForFile(session.json_data, df.path, repoRoot);
            if (ranges.empty()) continue;
            if (stagedRangeIt != stagedRanges.added.end()) {
                ranges = ranges.intersect(stagedRangeIt->second);
            }
            if (ranges.empty()) continue;
            sessionAttributedRanges = sessionAttributedRanges.unite(ranges);
            if (firstMatchingSession == nullptr) {
                firstMatchingSession = &session;
            }
        }

        if (!sessionAttributedRanges.empty() && firstMatchingSession != nullptr) {
            pred.predictedAiAdditions = static_cast<int>(sessionAttributedRanges.lineCount());
            pred.reason = "captured by " + firstMatchingSession->agent + "/" + firstMatchingSession->model;
            pred.basis = "uncommitted_session";
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
                    pred.reason = "continues existing AI-attributed file";
                    pred.basis = "head_note_history";
                } else {
                    pred.predictedAiAdditions = 0;
                    pred.reason = "no captured AI attribution";
                    pred.basis = "none";
                }
            } else {
                pred.predictedAiAdditions = 0;
                pred.reason = "no captured AI attribution";
                pred.basis = "none";
            }
        }
        predictedAiAdditions += pred.predictedAiAdditions;
        predictions.push_back(pred);
    }

    double aiPercent = totalAdditions > 0 ? (predictedAiAdditions * 100.0) / totalAdditions : 0.0;

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
        std::cout << "  \"basis\": \"uncommitted_sessions_then_head_notes\",\n";
        std::cout << "  \"staged_files\": " << stagedFiles.size() << ",\n";
        std::cout << "  \"ignored_staged_files\": " << ignoredStagedFiles << ",\n";
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
            std::cout << "    {\"path\": \"" << escapeJsonString(p.path) << "\", ";
            std::cout << "\"additions\": " << p.additions << ", ";
            std::cout << "\"deletions\": " << p.deletions << ", ";
            std::cout << "\"predicted_ai_additions\": " << p.predictedAiAdditions << ", ";
            std::cout << "\"basis\": \"" << escapeJsonString(p.basis) << "\", ";
            std::cout << "\"reason\": \"" << escapeJsonString(p.reason) << "\"}";
            if (i + 1 < predictions.size()) std::cout << ",";
            std::cout << "\n";
        }
        std::cout << "  ]\n";
        std::cout << "}\n";
    } else {
        std::cout << Style::header("check");
        std::cout << "  " << Style::dim("Staged changes only. Commit attribution is verified after commit.") << "\n\n";

        auto v = Style::violet;
        auto d = Style::dim;

        std::cout << Style::subHeader("Summary");
        std::cout << "  " << Style::padRight(Style::dim("files"), 18) << Style::glow(std::to_string(stagedFiles.size())) << "\n";
        if (ignoredStagedFiles > 0) {
            std::cout << "  " << Style::padRight(Style::dim("ignored"), 18) << Style::dim(std::to_string(ignoredStagedFiles)) << "\n";
        }
        std::cout << "  " << Style::padRight(Style::dim("sessions"), 18) << Style::glow(std::to_string(uncommittedSessions.size())) << "\n";
        std::cout << "  " << Style::padRight(Style::dim("additions"), 18) << Style::success("+" + std::to_string(totalAdditions)) << "\n";
        std::cout << "  " << Style::padRight(Style::dim("ai predicted"), 18) << (predictedAiAdditions > 0 ? Style::success("+" + std::to_string(predictedAiAdditions)) : d("0")) << "\n";
        std::cout << "  " << Style::padRight(Style::dim("ai share"), 18) << v(std::to_string(static_cast<int>(aiPercent)) + "%") << "\n";

        std::cout << "\n" << Style::subHeader("Files");
        for (const auto& p : predictions) {
            int pct = p.additions > 0 ? std::min((p.predictedAiAdditions * 100) / p.additions, 100) : 0;
            std::cout << "  " << Style::padRight(Style::blue(p.path), 34);
            std::cout << Style::padRight(std::to_string(p.additions) + "+ " + std::to_string(p.deletions) + "-", 10);
            std::cout << Style::progressBar(pct, 100, 8) << " ";
            std::cout << d(p.reason);
            std::cout << "\n";
        }

        std::cout << "\n" << Style::subHeader("Policy");
        std::cout << "  " << Style::padRight(d("threshold"), 18) << d(std::to_string(cfg.threshold) + "%") << "\n";
        std::cout << "  " << Style::padRight(d("result"), 18);
        if (wouldPass) {
            std::cout << Style::success(statusMsg);
        } else {
            std::cout << Style::error(statusMsg);
        }
        std::cout << "\n";
        std::cout << "  " << d("Run 'ghost audit' after committing for durable attribution.") << "\n\n";
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

static int handleExplain(int argc, char* argv[]) {
    using namespace ghost::output;

    std::string topic;
    if (argc > 2 && std::string(argv[2])[0] != '-') {
        topic = lowerString(argv[2]);
    }

    auto printTopic = [&](const std::string& name,
                          const std::string& stage,
                          const std::vector<std::string>& reads,
                          const std::vector<std::string>& doesNotRead,
                          const std::string& enforces,
                          const std::string& next) {
        std::cout << Style::header("Explain: " + name);
        std::cout << "  " << Style::label("stage") << "       " << Style::violet(stage) << "\n\n";

        std::cout << "  " << Style::subHeader("Reads");
        for (const auto& item : reads) {
            std::cout << "    " << Style::dim("- ") << item << "\n";
        }
        std::cout << "\n";

        if (!doesNotRead.empty()) {
            std::cout << "  " << Style::subHeader("Does Not Read");
            for (const auto& item : doesNotRead) {
                std::cout << "    " << Style::dim("- ") << item << "\n";
            }
            std::cout << "\n";
        }

        std::cout << "  " << Style::subHeader("Policy");
        std::cout << "    " << enforces << "\n\n";

        std::cout << "  " << Style::subHeader("Next");
        std::cout << "    " << next << "\n\n";
    };

    if (topic.empty()) {
        std::cout << Style::header("Explain");
        std::cout << "  " << Style::dim("Choose a command to explain:\n\n");
        std::cout << "    ghost explain init\n";
        std::cout << "    ghost explain status\n";
        std::cout << "    ghost explain check\n";
        std::cout << "    ghost explain audit\n";
        std::cout << "    ghost explain verify-pr\n";
        std::cout << "    ghost explain policy\n\n";
        return GHOST_EXIT_OK;
    }

    if (topic == "init") {
        printTopic(
            "init",
            "setup",
            {
                "git repository root",
                "current git user.email",
                "existing ghost.yml when present",
                "detected local project directories for ignore defaults"
            },
            {
                "committed attribution notes",
                "pull request state"
            },
            "Creates maintainer policy with --owner, or installs local compliance without changing policy with --contributor.",
            "Maintainers should run 'ghost init --owner'; contributors should run 'ghost init --contributor'."
        );
        return GHOST_EXIT_OK;
    }

    if (topic == "status") {
        printTopic(
            "status",
            "current working state",
            {
                "ghost.yml in the current repo",
                "local git hooks",
                "staged and unstaged working tree counts",
                "uncommitted Ghost agent sessions under .git/ghost",
                "HEAD ghost and ghost-verified notes"
            },
            {
                "full committed history",
                "pull request base-branch policy",
                "unstaged file attribution in ghost check"
            },
            "Does not enforce policy; it explains what exists now.",
            "Use 'ghost check' after git add, and 'ghost audit' after commit."
        );
        return GHOST_EXIT_OK;
    }

    if (topic == "check") {
        printTopic(
            "check",
            "pre-commit preview",
            {
                "staged diff only",
                "uncommitted Ghost sessions",
                "open checkpoint state",
                "HEAD notes for existing line attribution",
                "current repo ghost.yml"
            },
            {
                "unstaged changes",
                "future commit notes",
                "pull request base-branch policy unless --config-ref is used"
            },
            "Predicts whether staged changes would exceed the local policy threshold.",
            "Run 'git add <files>' first, then 'ghost check', then commit."
        );
        return GHOST_EXIT_OK;
    }

    if (topic == "audit") {
        printTopic(
            "audit",
            "committed codebase attribution",
            {
                "HEAD codebase attribution by default",
                "committed Git history when --range or --all is passed",
                "refs/notes/ghost",
                "refs/notes/ghost-verified",
                "refs/notes/ai when git-ai fallback is enabled",
                "ghost.yml, or base-branch ghost.yml when --config-ref is passed"
            },
            {
                "uncommitted sessions",
                "unstaged working tree changes",
                "staged changes that have not been committed"
            },
            "Shows the final policy result for committed code at HEAD and lists AI-touched files in one table.",
            "Use 'ghost verify-pr' before pushing. Use 'ghost audit --range BASE..HEAD --config-ref origin/main' only when you want historical commit context."
        );
        return GHOST_EXIT_OK;
    }

    if (topic == "verify-pr" || topic == "verify") {
        printTopic(
            "verify-pr",
            "local PR simulation",
            {
                "the final diff for the selected PR range, defaulting to origin/main..HEAD",
                "base-branch ghost.yml, defaulting to origin/main:ghost.yml",
                "Ghost notes for commits that authored surviving final-diff lines",
                "historical commits as warnings unless enforcement.history is block"
            },
            {
                "GitHub review approvals",
                "CODEOWNERS approval state",
                "uncommitted local changes"
            },
            "Enforces the final PR diff by default; intermediate commit history is context unless configured to block.",
            "Run before pushing: 'ghost verify-pr origin/main..HEAD'."
        );
        return GHOST_EXIT_OK;
    }

    if (topic == "policy") {
        printTopic(
            "policy",
            "owner controls",
            {
                "ghost.yml owner and owners allowlist",
                "policy mode",
                "locked flag",
                "threshold and verification policy",
                "banished ignore paths"
            },
            {
                "committed attribution percentages",
                "PR review approvals"
            },
            "Shows who can change protected policy and whether policy is locked.",
            "Owners can use 'ghost policy set mode <mode>', 'ghost policy lock', or 'ghost policy unlock --force'."
        );
        return GHOST_EXIT_OK;
    }

    std::cerr << Style::error("Unknown explain topic: " + topic + "\n")
              << Style::dim("  Try: init, status, check, audit, verify-pr, policy\n");
    return GHOST_EXIT_ERROR;
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
    } else if (command == "uninstall") {
        return handleUninstall(argc, argv);
    } else if (command == "install-hooks") {
        return handleInstallHooks(argc, argv);
    } else if (command == "uninstall-hooks") {
        return handleUninstallHooks(argc, argv);
    } else if (command == "audit") {
        return handleAudit(argc, argv);
    } else if (command == "verify-pr") {
        return handleVerifyPr(argc, argv);
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
    } else if (command == "policy") {
        return handlePolicy(argc, argv);
    } else if (command == "notes") {
        return handleNotes(argc, argv);
    } else if (command == "banish") {
        return handleBanish(argc, argv);
    } else if (command == "doctor") {
        return handleDoctor(argc, argv);
    } else if (command == "status") {
        return handleStatus(argc, argv);
    } else if (command == "explain") {
        return handleExplain(argc, argv);
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
