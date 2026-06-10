#include "installer.hpp"
#include "agent_hooks.hpp"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <iostream>
#include <cstdio>
#include <memory>
#include <vector>
#include <ctime>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace ghost {
namespace hooks {

static std::string getHomeDir() {
    const char* home = std::getenv("USERPROFILE");
    if (!home) home = std::getenv("HOME");
    return home ? home : "";
}

static std::string getBinDir() {
    return getHomeDir() + "/.ghost/bin";
}

static std::string getCurrentExeDir() {
    char path[4096];
#ifdef _WIN32
    DWORD len = GetModuleFileNameA(NULL, path, sizeof(path));
    if (len == 0 || len == sizeof(path)) return "";
#else
    ssize_t len = readlink("/proc/self/exe", path, sizeof(path));
    if (len == -1 || len == static_cast<ssize_t>(sizeof(path))) return "";
    path[len] = '\0';
#endif
    return fs::path(path).parent_path().string();
}

static std::string runCommand(const std::string& cmd) {
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) return "";
    std::string result;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe.get())) result += buffer;
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) result.pop_back();
    return result;
}

static bool copyFile(const std::string& src, const std::string& dst) {
    std::error_code ec;
    if (fs::exists(src, ec) && fs::exists(dst, ec) && fs::equivalent(src, dst, ec)) {
        return true;
    }
    fs::create_directories(fs::path(dst).parent_path(), ec);
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
#ifndef _WIN32
    if (!ec) {
        fs::permissions(
            dst,
            fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
            fs::perm_options::add,
            ec
        );
    }
#endif
    return !ec;
}

static std::vector<fs::path> legacyRepoOpenCodePluginPaths(const fs::path& repoRoot) {
    fs::path opencodeDir = repoRoot / ".opencode";
    return {
        opencodeDir / "plugins" / "ghost.ts",
        opencodeDir / "plugin" / "ghost.ts",
    };
}

static const char* POST_COMMIT_HOOK = R"(#!/bin/sh
GHOST="${GHOST_BIN:+$GHOST_BIN/ghost}"
GHOST="${GHOST:-$HOME/.ghost/bin/ghost}"
"$GHOST" post-commit 2>/dev/null || true
)";

static const char* PRE_PUSH_HOOK = R"HOOK(#!/bin/sh
REPO_ROOT="$(git rev-parse --show-toplevel)"
GHOST="${GHOST_BIN:+$GHOST_BIN/ghost}"
GHOST="${GHOST:-$HOME/.ghost/bin/ghost}"

GHOST_YML="$REPO_ROOT/ghost.yml"
GHOST_REQUIRED="false"
if [ -f "$GHOST_YML" ]; then
    if grep -q '^required: *true' "$GHOST_YML" 2>/dev/null; then
        GHOST_REQUIRED="true"
    fi
fi

if [ "$GHOST_REQUIRED" != "true" ]; then
    exit 0
fi

BASE_REF="$(git symbolic-ref --quiet --short refs/remotes/origin/HEAD 2>/dev/null)"
if [ -n "$BASE_REF" ]; then
    BASE_REF="origin/${BASE_REF#origin/}"
else
    BASE_REF="origin/main"
fi

if ! git rev-parse --verify "$BASE_REF" >/dev/null 2>&1; then
    BASE_REF=""
fi

BLOCKED=""

while read local_ref local_oid remote_ref remote_oid
do
    if [ "$local_oid" = "0000000000000000000000000000000000000000" ]; then
        continue
    fi

    case "$local_ref:$remote_ref" in
        refs/notes/*:*|*:refs/notes/*|refs/tags/*:*|*:refs/tags/*)
            continue
            ;;
    esac

    case "$local_ref:$remote_ref" in
        refs/heads/*:*|*:refs/heads/*)
            ;;
        *)
            continue
            ;;
    esac

    CHECK_BASE="$BASE_REF"
    if [ -z "$CHECK_BASE" ]; then
        if [ "$remote_oid" = "0000000000000000000000000000000000000000" ]; then
            continue
        fi
        CHECK_BASE="$remote_oid"
    fi

    echo ""
    echo "Ghost pre-push: verifying final branch diff against $CHECK_BASE"
    if ! "$GHOST" verify-pr "$CHECK_BASE..$local_oid" --base "$CHECK_BASE" --no-fetch; then
        BLOCKED="1"
    fi
done

if [ -n "$BLOCKED" ]; then
    echo ""
    echo "Ghost blocked this push because the final branch diff does not satisfy policy."
    echo "Run: ghost verify-pr --base ${BASE_REF:-origin/main}"
    exit 1
fi

exit 0
)HOOK";

static const char* POST_REWRITE_HOOK = R"HOOK(#!/bin/sh
# Ghost post-rewrite hook: rebase, amend, etc.
GHOST="${GHOST_BIN:+$GHOST_BIN/ghost}"
GHOST="${GHOST:-$HOME/.ghost/bin/ghost}"

# stdin contains old-sha new-sha pairs (one per line)
"$GHOST" rewrite-log --stdin 2>/dev/null || true
)HOOK";

static const char* POST_MERGE_HOOK = R"HOOK(#!/bin/sh
# Ghost post-merge hook
GHOST="${GHOST_BIN:+$GHOST_BIN/ghost}"
GHOST="${GHOST:-$HOME/.ghost/bin/ghost}"

REPO_ROOT="$(git rev-parse --show-toplevel)"

if [ "$GHOST_MERGE_SQUASH" = "1" ]; then
    "$GHOST" working-state --save --key merge_squash --repo "$REPO_ROOT" 2>/dev/null || true
fi

"$GHOST" rewrite-log --event merge --repo "$REPO_ROOT" 2>/dev/null || true
)HOOK";

static const char* POST_CHECKOUT_HOOK = R"HOOK(#!/bin/sh
# Ghost post-checkout hook: detect stash pop, branch switch
GHOST="${GHOST_BIN:+$GHOST_BIN/ghost}"
GHOST="${GHOST:-$HOME/.ghost/bin/ghost}"

REPO_ROOT="$(git rev-parse --show-toplevel)"
PREV_HEAD="$1"
NEW_HEAD="$2"
CHECKOUT_TYPE="$3"

if [ "$CHECKOUT_TYPE" = "0" ]; then
    # File-level checkout (e.g., git checkout -- <file>) -- ignore
    exit 0
fi

"$GHOST" rewrite-log --event checkout --repo "$REPO_ROOT" --prev "$PREV_HEAD" --new "$NEW_HEAD" 2>/dev/null || true
)HOOK";

static const char* PRE_MERGE_COMMIT_HOOK = R"HOOK(#!/bin/sh
# Ghost pre-merge-commit hook: save working state before merge commit
GHOST="${GHOST_BIN:+$GHOST_BIN/ghost}"
GHOST="${GHOST:-$HOME/.ghost/bin/ghost}"

REPO_ROOT="$(git rev-parse --show-toplevel)"
"$GHOST" working-state --save --key merge_commit --repo "$REPO_ROOT" 2>/dev/null || true
)HOOK";

int Installer::installBin() {
    std::string binDir = getBinDir();
    std::error_code ec;
    fs::create_directories(binDir, ec);

    std::string exeDir = getCurrentExeDir();
    if (exeDir.empty()) {
        std::cerr << "Could not determine Ghost binary location\n";
        return 1;
    }

#ifdef _WIN32
    std::string ghostName = "ghost.exe";
    std::string checkpointName = "ghost-checkpoint.exe";
#else
    std::string ghostName = "ghost";
    std::string checkpointName = "ghost-checkpoint";
#endif
    std::string ghostSrc = (fs::path(exeDir) / ghostName).string();
    std::string checkpointSrc = (fs::path(exeDir) / checkpointName).string();

    bool ok = true;

    if (fs::exists(ghostSrc, ec)) {
        if (copyFile(ghostSrc, (fs::path(binDir) / ghostName).string())) {
            std::cout << "  Installed " << ghostName << " to " << binDir << "\n";
        } else {
            std::cerr << "  Failed to copy " << ghostName << "\n";
            ok = false;
        }
    } else {
        std::cerr << "  " << ghostName << " not found at " << ghostSrc << "\n";
        ok = false;
    }

    if (fs::exists(checkpointSrc, ec)) {
        if (copyFile(checkpointSrc, (fs::path(binDir) / checkpointName).string())) {
            std::cout << "  Installed " << checkpointName << " to " << binDir << "\n";
        } else {
            std::cerr << "  Failed to copy " << checkpointName << "\n";
            ok = false;
        }
    } else {
        std::cerr << "  " << checkpointName << " not found at " << checkpointSrc << "\n";
        ok = false;
    }

    return ok ? 0 : 1;
}

int Installer::installRepo(const std::string& repoRoot) {
    fs::path root(repoRoot);

    std::cout << "Installing Ghost in " << repoRoot << "\n";

    std::error_code ec;

    std::string hooksDir = (root / ".git" / "hooks").string();
    fs::create_directories(hooksDir, ec);
    std::string hookPath = hooksDir + "/post-commit";
    std::ofstream hookFile(hookPath);
    if (hookFile.is_open()) {
        hookFile << POST_COMMIT_HOOK;
        hookFile.close();
#ifndef _WIN32
        runCommand("chmod +x \"" + hookPath + "\"");
#endif
        std::cout << "  Created .git/hooks/post-commit\n";
    } else {
        std::cerr << "  Failed to create post-commit hook\n";
        return 1;
    }

    std::string prePushPath = hooksDir + "/pre-push";
    std::ofstream prePushFile(prePushPath);
    if (prePushFile.is_open()) {
        prePushFile << PRE_PUSH_HOOK;
        prePushFile.close();
#ifndef _WIN32
        runCommand("chmod +x \"" + prePushPath + "\"");
#endif
        std::cout << "  Created .git/hooks/pre-push\n";
    } else {
        std::cerr << "  Failed to create pre-push hook\n";
        return 1;
    }

    // New hooks for history rewriting preservation
    auto installHook = [&](const std::string& name, const char* content) {
        std::string path = hooksDir + "/" + name;
        std::ofstream f(path);
        if (f.is_open()) {
            f << content;
            f.close();
#ifndef _WIN32
            runCommand("chmod +x \"" + path + "\"");
#endif
            std::cout << "  Created .git/hooks/" << name << "\n";
            return true;
        } else {
            std::cerr << "  Failed to create .git/hooks/" << name << "\n";
            return false;
        }
    };

    installHook("post-rewrite", POST_REWRITE_HOOK);
    installHook("post-merge", POST_MERGE_HOOK);
    installHook("post-checkout", POST_CHECKOUT_HOOK);
    installHook("pre-merge-commit", PRE_MERGE_COMMIT_HOOK);

    std::string existing = runCommand("git config --get-all remote.origin.push 2>&1");
    auto addOnce = [&](const std::string& ref) {
        if (existing.find(ref) == std::string::npos) {
            runCommand("git config --add remote.origin.push " + ref + " 2>&1");
        }
    };
    addOnce("refs/notes/ghost");
    addOnce("refs/notes/ghost-verified");
    addOnce("refs/notes/ghost-signatures");
    std::cout << "  Configured notes push\n";

    // Bootstrap step: detect unpushed commits without ghost notes
    {
        std::string unpushed = runCommand("git log --branches --not --remotes --format=%H");
        if (!unpushed.empty()) {
            std::istringstream commits(unpushed);
            std::string sha;
            std::vector<std::string> missingNotes;
            int totalUnpushed = 0;
            
            while (std::getline(commits, sha)) {
                if (sha.empty()) continue;
                totalUnpushed++;
                std::string note = runCommand("git notes --ref=refs/notes/ghost show " + sha + " 2>/dev/null");
                if (note.empty()) {
                    missingNotes.push_back(sha);
                }
            }
            
            if (!missingNotes.empty()) {
                std::cout << "\n  Bootstrap: " << missingNotes.size() << " of " << totalUnpushed
                          << " unpushed commit(s) have no ghost notes.\n";
                std::cout << "  These commits will be permanently recorded as human-authored.\n";
                
                // Ask for confirmation in interactive mode
                bool confirmed = true;
                if (
#ifdef _WIN32
                    _isatty(_fileno(stdin)) && _isatty(_fileno(stdout))
#else
                    isatty(fileno(stdin)) && isatty(fileno(stdout))
#endif
                ) {
                    std::cout << "\n  Confirm and continue? [y/N]: ";
                    std::string response;
                    std::getline(std::cin, response);
                    confirmed = (response == "y" || response == "Y" || response == "yes");
                }
                
                if (confirmed) {
                    std::string ghostDir = (root / ".git" / "ghost").string();
                    fs::create_directories(ghostDir, ec);
                    std::string logPath = ghostDir + "/bootstrap.log";
                    std::ofstream log(logPath, std::ios::app);
                    if (log.is_open()) {
                        time_t now = std::time(nullptr);
                        log << "[" << now << "] Bootstrap confirmed. "
                            << missingNotes.size() << " commits without ghost notes:\n";
                        for (const auto& s : missingNotes) {
                            log << "  " << s << "\n";
                        }
                        log.close();
                        std::cout << "  Bootstrap log written to .git/ghost/bootstrap.log\n";
                    }
                } else {
                    std::cout << "  Bootstrap cancelled.\n";
                }
            }
        }
    }

    std::cout << "Done. Ghost repo policy and Git hooks are installed.\n";
    return 0;
}

int Installer::installGlobal() {
    bool ok = true;
    for (const auto& agent : {"opencode", "codex", "claude", "cursor", "antigravity"}) {
        if (!AgentHooks::installForAgent("", agent, true)) {
            ok = false;
        }
    }

    if (ok) {
        std::cout << "Done. Ghost will track AI edits for supported agents globally.\n";
    }
    return ok ? 0 : 1;
}

int Installer::uninstallRepo(const std::string& repoRoot) {
    fs::path root(repoRoot);
    std::error_code ec;

    for (const auto& pluginPath : legacyRepoOpenCodePluginPaths(root)) {
        if (fs::remove(pluginPath, ec)) {
            std::cout << "  Removed " << fs::relative(pluginPath, root).string() << "\n";
        }
    }
    fs::remove((root / ".opencode" / "plugin"), ec);

    std::string hookPath = (root / ".git" / "hooks" / "post-commit").string();
    if (fs::remove(hookPath, ec)) {
        std::cout << "  Removed .git/hooks/post-commit\n";
    }

    std::string prePushPath = (root / ".git" / "hooks" / "pre-push").string();
    if (fs::remove(prePushPath, ec)) {
        std::cout << "  Removed .git/hooks/pre-push\n";
    }

    // Remove new history-rewriting hooks
    auto removeHook = [&](const std::string& name) {
        std::string path = (root / ".git" / "hooks" / name).string();
        if (fs::remove(path, ec)) {
            std::cout << "  Removed .git/hooks/" << name << "\n";
        }
    };
    removeHook("post-rewrite");
    removeHook("post-merge");
    removeHook("post-checkout");
    removeHook("pre-merge-commit");

    std::cout << "Done. Ghost uninstalled from this repo.\n";
    return 0;
}

int Installer::uninstallGlobal() {
    bool ok = true;
    for (const auto& agent : {"opencode", "codex", "claude", "cursor", "antigravity"}) {
        if (!AgentHooks::uninstallForAgent("", agent, true)) {
            ok = false;
        }
    }
    std::cout << "Done. Ghost global agent hooks removed.\n";
    return ok ? 0 : 1;
}

}
}
