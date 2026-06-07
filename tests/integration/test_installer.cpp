#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <memory>
#include <string>
#include <chrono>

namespace fs = std::filesystem;

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

// TODO: Add tests that invoke the ghost binary:
// - Test ghost init creates .opencode/plugins/ghost.ts
// - Test ghost init creates .git/hooks/post-commit
// - Test ghost uninstall removes both
// - Test git config remote.origin.push has refs/notes/ghost
