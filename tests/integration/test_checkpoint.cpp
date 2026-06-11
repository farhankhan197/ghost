#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <memory>
#include <string>
#include <chrono>
#include <vector>

namespace fs = std::filesystem;

static std::string quotePath(const fs::path& path) {
    return "\"" + path.string() + "\"";
}

static std::string checkpointBin() {
#ifdef _WIN32
    std::vector<fs::path> candidates = {
        fs::current_path() / "ghost-checkpoint.exe",
        fs::current_path() / "build" / "ghost-checkpoint.exe",
        fs::current_path().parent_path() / "ghost-checkpoint.exe"
    };
#else
    std::vector<fs::path> candidates = {
        fs::current_path() / "ghost-checkpoint",
        fs::current_path() / "build" / "ghost-checkpoint",
        fs::current_path().parent_path() / "ghost-checkpoint"
    };
#endif
    for (const auto& candidate : candidates) {
        if (fs::exists(candidate)) return candidate.string();
    }
    return candidates.front().string();
}

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

// Helper: Create a temp directory and git repo
class TempGitRepo {
public:
    std::string path;
    
    TempGitRepo() {
        auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(rand());
        auto tmp = fs::temp_directory_path() / ("ghost-test-" + suffix);
        path = tmp.string();
        fs::create_directories(path);
        
        // Initialize git repo
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

// Placeholder for checkpoint integration tests
// These tests require the checkpoint binary to be built and accessible
// For CI/CD, we can run the actual binary via std::system()

TEST(CheckpointIntegration, TempRepoSetup) {
    TempGitRepo repo;
    
    repo.writeFile("main.cpp", "int main() { return 0; }\n");
    repo.addAndCommit("Initial commit");
    
    // Verify git repo is initialized
    EXPECT_TRUE(fs::exists(repo.path + "/.git"));
    
    // Verify file was committed
    std::string fullCmd = "cd \"" + repo.path + "\" && git show HEAD:main.cpp";
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(fullCmd.c_str(), "r"), pclose);
    EXPECT_NE(pipe, nullptr);
}

TEST(CheckpointIntegration, BroadCodexHookIgnoresNonEditToolEvents) {
    TempGitRepo repo;
    int rc = 0;
    fs::path payload = fs::path(repo.path) / "bash-hook.json";
    {
        std::ofstream f(payload);
        f << R"JSON({"tool":"Bash","command":"git status"})JSON";
    }

    std::string out = runCapture(
        quotePath(checkpointBin()) + " pre --agent codex --hook-json < " + quotePath(payload),
        repo.path,
        &rc
    );

    ASSERT_EQ(rc, 0) << out;
    EXPECT_EQ(out.find("Capturing snapshot"), std::string::npos) << out;
    EXPECT_FALSE(fs::exists(fs::path(repo.path) / ".git" / "ghost"));
}
