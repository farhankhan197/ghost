#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <memory>
#include <string>

namespace fs = std::filesystem;

class TempGitRepo {
public:
    std::string path;
    
    TempGitRepo() {
        auto tmp = fs::temp_directory_path() / ("ghost-test-" + std::to_string(rand()));
        path = tmp.string();
        fs::create_directories(path);
        
        runCommand("git init", path);
        runCommand("git config user.name \"Test User\"", path);
        runCommand("git config user.email \"test@test.com\"", path);
    }
    
    ~TempGitRepo() {
        if (fs::exists(path)) {
            std::string cmd = "rm -rf \"" + path + "\"";
            std::system(cmd.c_str());
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
    
    std::string getHeadSha() {
        std::string fullCmd = "cd \"" + path + "\" && git rev-parse HEAD";
        std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(fullCmd.c_str(), "r"), pclose);
        if (!pipe) return "";
        std::string result;
        char buffer[256];
        while (fgets(buffer, sizeof(buffer), pipe.get())) result += buffer;
        while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) result.pop_back();
        return result;
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

// Placeholder for audit integration tests
// Will test: blame overlay + aggregation + policy enforcement

TEST(AuditIntegration, TempRepoWithCommits) {
    TempGitRepo repo;
    
    repo.writeFile("main.cpp", "int main() { return 0; }\n");
    repo.addAndCommit("Initial commit");
    
    std::string sha1 = repo.getHeadSha();
    EXPECT_EQ(sha1.length(), 40);  // Full SHA
    
    // Modify existing file to ensure commit is not empty
    repo.writeFile("main.cpp", "int main() { return 1; }\n");
    repo.addAndCommit("Update main");
    
    std::string sha2 = repo.getHeadSha();
    EXPECT_NE(sha1, sha2);
    
    // Add new file
    repo.writeFile("lib.cpp", "void helper() {}\n");
    repo.addAndCommit("Add helper");
    
    std::string sha3 = repo.getHeadSha();
    EXPECT_NE(sha2, sha3);
}

TEST(AuditIntegration, NoGhostNotes) {
    TempGitRepo repo;
    
    repo.writeFile("main.cpp", "int main() { return 0; }\n");
    repo.addAndCommit("Initial commit");
    
    // Verify no ghost notes exist
    std::string fullCmd = "cd \"" + repo.path + "\" && git notes --ref=ghost list 2>&1";
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(fullCmd.c_str(), "r"), pclose);
    if (pipe) {
        char buffer[256];
        std::string result;
        while (fgets(buffer, sizeof(buffer), pipe.get())) result += buffer;
        // Should be empty or error (no notes)
        EXPECT_TRUE(result.empty() || result.find("error") != std::string::npos);
    }
}
