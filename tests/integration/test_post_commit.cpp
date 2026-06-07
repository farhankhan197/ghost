#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <memory>
#include <string>
#include <chrono>

namespace fs = std::filesystem;

// Helper: Create a temp directory and git repo
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
    
    void writeFile(const std::string& filePath, const std::string& content) {
        fs::create_directories(fs::path(path) / fs::path(filePath).parent_path());
        std::ofstream f(path + "/" + filePath);
        f << content;
        f.close();
    }
    
    void addAndCommit(const std::string& msg) {
        runCommand("git add -A", path);
        runCommand("git commit -m \"" + msg + "\"", path);
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

// Placeholder for post-commit integration tests
// Will test: session JSON → ghost note creation → cleanup

TEST(PostCommitIntegration, TempRepoSetup) {
    TempGitRepo repo;
    
    repo.writeFile("main.cpp", "int main() { return 0; }\n");
    repo.addAndCommit("Initial commit");
    
    // Verify repo is set up
    EXPECT_TRUE(fs::exists(repo.path + "/.git"));
    EXPECT_TRUE(fs::exists(repo.path + "/main.cpp"));
}
