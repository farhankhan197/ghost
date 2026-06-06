#include "installer.hpp"
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
    fs::create_directories(fs::path(dst).parent_path(), ec);
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    return !ec;
}

static const char* PLUGIN_CONTENT = R"(export const GhostPlugin = async ({ $, directory, worktree }) => {
  let currentModel = "unknown"

  function getBinDir() {
    if (process.env.GHOST_BIN) return process.env.GHOST_BIN
    const home = process.env.USERPROFILE || process.env.HOME || ""
    return home + "/.ghost/bin"
  }

  function getCheckpointPath() {
    const bin = getBinDir()
    return process.platform === "win32"
      ? bin.replace(/\//g, "\\") + "\\ghost-checkpoint.exe"
      : bin + "/ghost-checkpoint"
  }

  function writeModelFile(model) {
    const home = process.env.USERPROFILE || process.env.HOME || ""
    const modelPath = home + "/.ghost/.current_model"
    try {
      const fs = require("fs")
      fs.writeFileSync(modelPath, model)
    } catch (e) {}
  }

  return {
    "session.updated": async ({ event }) => {
      if (event?.model) {
        const parts = event.model.split("/")
        currentModel = parts.length > 1 ? parts[1] : event.model
        writeModelFile(currentModel)
      }
    },
    "tool.execute.before": async (input, output) => {
      if (input.tool === "edit" || input.tool === "write" || input.tool === "apply_patch") {
        const cp = getCheckpointPath()
        const filePath = input?.path || input?.file || (input?.files && input.files[0]) || ""
        if (filePath) {
          await $`${cp} pre --agent opencode --file ${filePath}`.quiet().catch(() => {})
        } else {
          await $`${cp} pre --agent opencode`.quiet().catch(() => {})
        }
      }
    },
    "tool.execute.after": async (input, output) => {
      if (input.tool === "edit" || input.tool === "write" || input.tool === "apply_patch") {
        const cp = getCheckpointPath()
        const filePath = input?.path || input?.file || (input?.files && input.files[0]) || ""
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

static const char* POST_COMMIT_HOOK = R"(#!/bin/sh
GHOST="${GHOST_BIN:+$GHOST_BIN/ghost}"
GHOST="${GHOST:-$HOME/.ghost/bin/ghost}"
"$GHOST" post-commit 2>/dev/null || true
)";

static const char* PRE_PUSH_HOOK = R"HOOK(#!/bin/sh
REPO_ROOT="$(git rev-parse --show-toplevel)"
GHOST_DIR="$REPO_ROOT/.git/ghost"
FIRST_PUSH_DIR="$GHOST_DIR/first_push"

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

MISSING_NOTES=""
HAS_COMMITS=""

while read local_ref local_oid remote_ref remote_oid
do
    if [ "$local_oid" = "0000000000000000000000000000000000000000" ]; then
        continue
    fi

    if [ "$remote_oid" = "0000000000000000000000000000000000000000" ]; then
        RANGE="$local_oid"
    else
        RANGE="$remote_oid..$local_oid"
    fi

    for commit in $(git rev-list "$RANGE" 2>/dev/null); do
        HAS_COMMITS="1"
        note=$(git notes --ref=refs/notes/ghost show "$commit" 2>/dev/null)
        if [ -z "$note" ]; then
            MISSING_NOTES="$MISSING_NOTES $commit"
        fi
    done
done

if [ -z "$HAS_COMMITS" ]; then
    exit 0
fi

if [ -z "$MISSING_NOTES" ]; then
    exit 0
fi

USER_EMAIL=$(git config user.email 2>/dev/null)
if [ -z "$USER_EMAIL" ]; then
    USER_EMAIL="unknown"
fi

SAFE_EMAIL=$(echo "$USER_EMAIL" | tr '/\\' '_')
FIRST_PUSH_FILE="$FIRST_PUSH_DIR/$SAFE_EMAIL"

if [ -f "$FIRST_PUSH_FILE" ]; then
    echo ""
    echo "ERROR: ghost attribution required but missing for commits:$MISSING_NOTES"
    echo ""
    echo "Install ghost and commit with attribution tracking enabled."
    echo "Run: ghost install"
    echo ""
    exit 1
fi

echo ""
echo "This repo uses ghost for code attribution."
echo "Some commits being pushed have no ghost notes:$MISSING_NOTES"
echo ""
echo "[1] Install ghost now (recommended)"
echo "[2] I confirm this code is human-written (one-time only)"
echo "[3] Cancel push"
echo ""

if [ -t 0 ] && [ -t 1 ]; then
    printf "Choose [1-3]: "
    read choice
else
    echo "Non-interactive environment detected. Install ghost to push."
    echo "Run: ghost install"
    exit 1
fi

case "$choice" in
    1)
        echo "Run 'ghost install' and commit your changes, then push again."
        exit 1
        ;;
    2)
        mkdir -p "$FIRST_PUSH_DIR"
        date +%Y-%m-%dT%H:%M:%S > "$FIRST_PUSH_FILE"
        echo "Confirmed. This is a one-time bypass."
        echo "Future pushes without ghost will be blocked."
        exit 0
        ;;
    *)
        echo "Push cancelled."
        exit 1
        ;;
esac
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
        std::cerr << "Could not determine ghost binary location\n";
        return 1;
    }

    std::string ghostSrc = exeDir + "/ghost.exe";
    std::string checkpointSrc = exeDir + "/ghost-checkpoint.exe";

    bool ok = true;

    if (fs::exists(ghostSrc, ec)) {
        if (copyFile(ghostSrc, binDir + "/ghost.exe")) {
            std::cout << "  Installed ghost.exe to " << binDir << "\n";
        } else {
            std::cerr << "  Failed to copy ghost.exe\n";
            ok = false;
        }
    } else {
        std::cerr << "  ghost.exe not found at " << ghostSrc << "\n";
        ok = false;
    }

    if (fs::exists(checkpointSrc, ec)) {
        if (copyFile(checkpointSrc, binDir + "/ghost-checkpoint.exe")) {
            std::cout << "  Installed ghost-checkpoint.exe to " << binDir << "\n";
        } else {
            std::cerr << "  Failed to copy ghost-checkpoint.exe\n";
            ok = false;
        }
    } else {
        std::cerr << "  ghost-checkpoint.exe not found at " << checkpointSrc << "\n";
        ok = false;
    }

    return ok ? 0 : 1;
}

int Installer::installRepo(const std::string& repoRoot) {
    fs::path root(repoRoot);

    std::cout << "Installing ghost in " << repoRoot << "\n";

    std::error_code ec;

    std::string pluginDir = (root / ".opencode" / "plugins").string();
    fs::create_directories(pluginDir, ec);
    std::string pluginPath = pluginDir + "/ghost.ts";
    std::ofstream pluginFile(pluginPath);
    if (pluginFile.is_open()) {
        pluginFile << PLUGIN_CONTENT;
        pluginFile.close();
        std::cout << "  Created .opencode/plugins/ghost.ts\n";
    } else {
        std::cerr << "  Failed to create plugin file\n";
        return 1;
    }

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

    std::cout << "Done. Ghost is now tracking AI edits in this repo.\n";
    return 0;
}

int Installer::installGlobal() {
    std::string configDir = getHomeDir() + "/.config/opencode/plugins";
    std::error_code ec;
    fs::create_directories(configDir, ec);

    std::string pluginPath = configDir + "/ghost.ts";
    std::ofstream pluginFile(pluginPath);
    if (pluginFile.is_open()) {
        pluginFile << PLUGIN_CONTENT;
        pluginFile.close();
        std::cout << "  Created ~/.config/opencode/plugins/ghost.ts\n";
    } else {
        std::cerr << "  Failed to create global plugin file\n";
        return 1;
    }

    std::cout << "Done. Ghost will track AI edits in all repos opened in opencode.\n";
    return 0;
}

int Installer::uninstallRepo(const std::string& repoRoot) {
    fs::path root(repoRoot);
    std::error_code ec;

    std::string pluginPath = (root / ".opencode" / "plugins" / "ghost.ts").string();
    if (fs::remove(pluginPath, ec)) {
        std::cout << "  Removed .opencode/plugins/ghost.ts\n";
    }

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
    std::string pluginPath = getHomeDir() + "/.config/opencode/plugins/ghost.ts";
    std::error_code ec;
    if (fs::remove(pluginPath, ec)) {
        std::cout << "  Removed ~/.config/opencode/plugins/ghost.ts\n";
    } else {
        std::cout << "  Global plugin not found\n";
    }
    std::cout << "Done. Ghost global plugin removed.\n";
    return 0;
}

}
}
