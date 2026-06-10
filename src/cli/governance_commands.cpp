#include "governance_commands.hpp"

#include "args.hpp"
#include "commands.hpp"
#include "exit_codes.hpp"
#include "audit/policy.hpp"
#include "config/ghost_config.hpp"
#include "git/command.hpp"
#include "git/repo.hpp"
#include "hooks/agent_detector.hpp"
#include "hooks/agent_hooks.hpp"
#include "hooks/installer.hpp"
#include "output/interactive.hpp"
#include "output/style.hpp"
#include "signing/ssh_signing.hpp"
#include "util/files.hpp"
#include "util/process.hpp"
#include "util/signature.hpp"
#include "util/text.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace ghost {
namespace cli {

static bool g_verbose = false;
static void logVerbose(const std::string& msg) {
    if (g_verbose) {
        std::cerr << output::Style::dim("[verbose] " + msg) << "\n";
    }
}

static constexpr int GHOST_EXIT_OK = kExitOk;
static constexpr int GHOST_EXIT_ERROR = kExitError;
static constexpr int GHOST_EXIT_BLOCKED = kExitBlocked;
static constexpr int GHOST_EXIT_NOT_IN_REPO = kExitNotInRepo;

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

static ghost::util::Process::Result runProcess(const std::string& executable, std::vector<std::string> args, const std::string& cwd = "") {
    ghost::util::Process::Command command;
    command.executable = executable;
    command.args = std::move(args);
    command.cwd = cwd;
    command.mergeStderr = true;
    return ghost::util::Process::capture(command);
}

static std::string captureProcess(const std::string& executable, std::vector<std::string> args, const std::string& cwd = "") {
    return runProcess(executable, std::move(args), cwd).stdoutText;
}

static std::string captureGit(std::vector<std::string> args, const std::string& cwd = "") {
    return git::Command::capture(cwd, std::move(args), "", true);
}

static std::string captureGh(std::vector<std::string> args, const std::string& cwd = "") {
    return captureProcess("gh", std::move(args), cwd);
}

static bool fileExists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

static std::string lowerString(const std::string& value) {
    return util::Text::lower(value);
}

static std::string trimString(const std::string& value) {
    return util::Text::trim(value);
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

static bool configureNotesRefs(const std::string& repoRoot) {
    std::string existing = captureGit({"config", "--get-all", "remote.origin.push"}, repoRoot);
    auto addOnce = [&](const std::string& ref) {
        if (existing.find(ref) == std::string::npos) {
            captureGit({"config", "--add", "remote.origin.push", ref}, repoRoot);
        }
    };
    addOnce("refs/notes/ghost");
    addOnce("refs/notes/ghost-verified");
    addOnce("refs/notes/ghost-signatures");
    return true;
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
    std::string url = captureGit({"remote", "get-url", "origin"});
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
    std::string url = captureGit({"remote", "get-url", "origin"});
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
    std::string login = captureGh({"api", "user", "--jq", ".login"});
    if (!login.empty() &&
        login.find("not found") == std::string::npos &&
        login.find("not recognized") == std::string::npos &&
        login.find("Not recognized") == std::string::npos &&
        login.find("error") == std::string::npos &&
        login.find("ERROR") == std::string::npos) {
        return trimString(login);
    }
    login = captureGit({"config", "github.user"});
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
    std::string permission = captureGh({
        "api",
        "repos/" + slug.owner + "/" + slug.repo + "/collaborators/" + login + "/permission",
        "--jq",
        ".permission"
    });
    permission = normalizeIdentityToken(permission);
    return permission == "admin" || permission == "maintain";
}

static std::string currentGitUserName() {
    return trimString(captureGit({"config", "user.name"}));
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

void printSuggestion(const std::string& unknown) {
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

int config(int argc, char* argv[]) {
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

int policy(int argc, char* argv[]) {
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
        std::string digest = ghost::util::hashFile(policyPath);
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
        auto sig = ghost::util::parseSimpleSignature(buffer.str());
        bool trustedRequired = hasFlag(argc, argv, "--trusted");
        std::string expected = sig["digest"];
        std::string actual = ghost::util::hashFile((std::filesystem::path(repoRoot) / "ghost.yml").string());
        if (expected.empty() || actual.empty() || expected != actual) {
            std::cerr << ghost::output::Style::error("Policy signature mismatch.\n")
                      << ghost::output::Style::dim("  ghost.yml digest: " + actual + "\n")
                      << ghost::output::Style::dim("  signed digest:    " + (expected.empty() ? "missing" : expected) + "\n");
            return GHOST_EXIT_BLOCKED;
        }
        if (sig["schema"] == "ghost-policy-signature/2") {
            long long ts = ghost::util::parseSignatureTs(sig);
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

int banish(int argc, char* argv[]) {
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

int init(int argc, char* argv[], bool verbose) {
    g_verbose = verbose;
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
        selectedAgents = ghost::hooks::AgentHooks::defaultCaptureAgents();
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

    configureNotesRefs(repoRoot);

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

} // namespace cli
} // namespace ghost
