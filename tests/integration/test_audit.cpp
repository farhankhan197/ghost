#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <memory>
#include <string>
#include <chrono>
#include <vector>
#include "git/notes.hpp"

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

TEST(AuditIntegration, BatchNotesFindRequestedCommitNote) {
    TempGitRepo repo;

    repo.writeFile("src/app.txt", "one\ntwo\n");
    repo.addAndCommit("AI file");
    std::string sha = repo.getHeadSha();

    std::string note =
        "src/app.txt\n"
        "  sess_test 1-2\n"
        "---\n"
        "{\n"
        "  \"schema\": \"ghost/1.0.0\",\n"
        "  \"commit\": \"" + sha + "\",\n"
        "  \"sessions\": {\n"
        "    \"sess_test\": {\n"
        "      \"session_id\": \"sess_test\",\n"
        "      \"agent\": \"opencode\",\n"
        "      \"model\": \"batch-model\",\n"
        "      \"author\": \"Test User <test@test.com>\",\n"
        "      \"ts_start\": 1,\n"
        "      \"ts_end\": 2,\n"
        "      \"additions\": 2,\n"
        "      \"deletions\": 0\n"
        "    }\n"
        "  }\n"
        "}\n";

    fs::path oldCwd = fs::current_path();
    fs::current_path(repo.path);
    bool wrote = ghost::git::Notes::write("refs/notes/ghost", sha, note);
    auto notes = ghost::git::Notes::showBatch("refs/notes/ghost", std::vector<std::string>{sha});
    fs::current_path(oldCwd);

    ASSERT_TRUE(wrote);
    ASSERT_EQ(notes.count(sha), 1u);
    EXPECT_NE(notes[sha].find("batch-model"), std::string::npos);
}
