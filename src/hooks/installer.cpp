#include "installer.hpp"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <iostream>
#include <cstdio>
#include <memory>
#include <windows.h>

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
    DWORD len = GetModuleFileNameA(NULL, path, sizeof(path));
    if (len == 0 || len == sizeof(path)) return "";
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

  return {
    "session.updated": async ({ event }) => {
      if (event?.model) {
        const parts = event.model.split("/")
        currentModel = parts.length > 1 ? parts[1] : event.model
      }
    },
    "tool.execute.before": async (input, output) => {
      if (input.tool === "edit" || input.tool === "write" || input.tool === "apply_patch") {
        const cp = getCheckpointPath()
        await $`${cp} pre --agent opencode`.quiet().catch(() => {})
      }
    },
    "tool.execute.after": async (input, output) => {
      if (input.tool === "edit" || input.tool === "write" || input.tool === "apply_patch") {
        const cp = getCheckpointPath()
        await $`${cp} post --agent opencode --model ${currentModel}`.quiet().catch(() => {})
      }
    },
  }
}
)";

static const char* POST_COMMIT_HOOK = "#!/bin/sh\nGHOST=\"${GHOST_BIN:+$GHOST_BIN/ghost}\"\nGHOST=\"${GHOST:-$HOME/.ghost/bin/ghost}\"\n\"$GHOST\" post-commit 2>/dev/null || true\n";

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

    std::string result1 = runCommand("git config --add remote.origin.push refs/notes/ghost 2>&1");
    std::string result2 = runCommand("git config --add remote.origin.push refs/notes/ghost-verified 2>&1");
    (void)result1;
    (void)result2;
    std::cout << "  Configured notes push\n";

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
