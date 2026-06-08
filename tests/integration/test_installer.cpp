#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <chrono>
#include <vector>

namespace fs = std::filesystem;

static std::string runCapture(const std::string& cmd, const std::string& cwd, int* exitCode = nullptr) {
    std::string fullCmd = "cd \"" + cwd + "\" && " + cmd + " 2>&1";
    FILE* pipe = popen(fullCmd.c_str(), "r");
    std::string result;
    if (pipe) {
        char buffer[1024];
        while (fgets(buffer, sizeof(buffer), pipe)) result += buffer;
        int rc = pclose(pipe);
        if (exitCode) *exitCode = rc;
    } else if (exitCode) {
        *exitCode = -1;
    }
    return result;
}

static std::string quotePath(const fs::path& path) {
    return "\"" + path.string() + "\"";
}

static std::string ghostBin() {
#ifdef _WIN32
    std::vector<fs::path> candidates = {
        fs::current_path() / "ghost.exe",
        fs::current_path() / "build" / "ghost.exe",
        fs::current_path().parent_path() / "ghost.exe"
    };
#else
    std::vector<fs::path> candidates = {
        fs::current_path() / "ghost",
        fs::current_path() / "build" / "ghost",
        fs::current_path().parent_path() / "ghost"
    };
#endif
    for (const auto& candidate : candidates) {
        if (fs::exists(candidate)) return candidate.string();
    }
    return candidates.front().string();
}

static std::string readText(const fs::path& path) {
    std::ifstream file(path);
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

class TempGitRepo {
public:
    std::string path;
    
    TempGitRepo() {
        auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(rand());
        auto tmp = fs::temp_directory_path() / ("ghost-test-" + suffix);
        path = tmp.string();
        fs::create_directories(path);
        
        runCommand("git init", path);
        runCommand("git config user.name \"Test User\"", path);
        runCommand("git config user.email \"test@test.com\"", path);
    }
    
    ~TempGitRepo() {
        if (fs::exists(path)) {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    }
    
private:
    void runCommand(const std::string& cmd, const std::string& cwd) {
        std::string fullCmd = "cd \"" + cwd + "\" && " + cmd + " 2>&1";
        std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(fullCmd.c_str(), "r"), pclose);
        if (pipe) {
            char buffer[256];
            while (fgets(buffer, sizeof(buffer), pipe.get())) {}
        }
    }
};

// Placeholder for installer integration tests
// Will test: ghost init creates plugin + hooks, ghost uninstall removes them

TEST(InstallerIntegration, TempRepoSetup) {
    TempGitRepo repo;
    
    // Verify .git directory exists
    EXPECT_TRUE(fs::exists(repo.path + "/.git"));
    
    // Verify .git/hooks directory exists
    EXPECT_TRUE(fs::exists(repo.path + "/.git/hooks"));
}

TEST(InstallerIntegration, InitInstallsCoreAgentCaptureHooks) {
    TempGitRepo repo;
    int rc = 0;

    std::string out = runCapture(
        quotePath(ghostBin()) + " init --owner --mode transparent --force",
        repo.path,
        &rc
    );
    ASSERT_EQ(rc, 0) << out;

    fs::path root(repo.path);
    EXPECT_TRUE(fs::exists(root / ".opencode" / "plugins" / "ghost.ts"));
    EXPECT_FALSE(fs::exists(root / ".opencode" / "plugin" / "ghost.ts"));
    EXPECT_TRUE(fs::exists(root / ".codex" / "hooks.json"));
    EXPECT_TRUE(fs::exists(root / ".claude" / "settings.json"));
    EXPECT_TRUE(fs::exists(root / ".cursor" / "hooks.json"));
    EXPECT_TRUE(fs::exists(root / ".agents" / "hooks.json"));

    std::string opencode = readText(root / ".opencode" / "plugins" / "ghost.ts");
    EXPECT_NE(opencode.find("tool.execute.before"), std::string::npos);
    EXPECT_NE(opencode.find("tool.execute.after"), std::string::npos);

    std::string codex = readText(root / ".codex" / "hooks.json");
    EXPECT_NE(codex.find("\"PreToolUse\""), std::string::npos);
    EXPECT_NE(codex.find("\"PostToolUse\""), std::string::npos);
    EXPECT_NE(codex.find("--agent codex"), std::string::npos);
    EXPECT_NE(codex.find("--hook-json"), std::string::npos);

    std::string claude = readText(root / ".claude" / "settings.json");
    EXPECT_NE(claude.find("\"PreToolUse\""), std::string::npos);
    EXPECT_NE(claude.find("\"PostToolUse\""), std::string::npos);
    EXPECT_NE(claude.find("Write|Edit|MultiEdit|ApplyDiff"), std::string::npos);
    EXPECT_NE(claude.find("--agent claude"), std::string::npos);
    EXPECT_NE(claude.find("--hook-json"), std::string::npos);

    std::string cursor = readText(root / ".cursor" / "hooks.json");
    EXPECT_NE(cursor.find("\"beforeFileEdit\""), std::string::npos);
    EXPECT_NE(cursor.find("\"afterFileEdit\""), std::string::npos);
    EXPECT_NE(cursor.find("--agent cursor"), std::string::npos);

    std::string antigravity = readText(root / ".agents" / "hooks.json");
    EXPECT_NE(antigravity.find("\"PreToolUse\""), std::string::npos);
    EXPECT_NE(antigravity.find("\"PostToolUse\""), std::string::npos);
    EXPECT_NE(antigravity.find("--agent antigravity"), std::string::npos);
}
