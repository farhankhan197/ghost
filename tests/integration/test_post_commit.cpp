#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <memory>
#include <string>
#include <chrono>
#include <sstream>
#include <vector>
#include "persist/db.hpp"

namespace fs = std::filesystem;

static std::string runCapture(const std::string& cmd, const std::string& cwd, int* exitCode = nullptr) {
    std::string fullCmd = "cd \"" + cwd + "\" && " + cmd + " 2>&1";
    FILE* pipe = popen(fullCmd.c_str(), "r");
    std::string result;
    if (pipe) {
        char buffer[512];
        while (fgets(buffer, sizeof(buffer), pipe)) result += buffer;
        int rc = pclose(pipe);
        if (exitCode) *exitCode = rc;
    } else if (exitCode) {
        *exitCode = -1;
    }
    return result;
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
        ghost::persist::closeRepoDb(path);
        if (fs::exists(path)) {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    }
    
    void writeFile(const std::string& filePath, const std::string& content) {
        fs::path parent = (fs::path(path) / fs::path(filePath)).parent_path();
        if (!parent.empty()) fs::create_directories(parent);
        std::ofstream f(path + "/" + filePath);
        f << content;
        f.close();
    }

    void writeSession(
        const std::string& sessionId,
        const std::string& filePath,
        const std::string& ranges,
        int additions
    ) {
        fs::create_directories(fs::path(path) / ".git" / "ghost");
        auto* db = ghost::persist::getRepoDb(path);
        ASSERT_NE(db, nullptr);

        ghost::persist::Session sess;
        sess.session_id = sessionId;
        sess.agent = "opencode";
        sess.model = "test-model";
        sess.author = "Test User <test@test.com>";
        sess.ts_start = 1;
        sess.ts_end = 2;
        sess.additions = additions;
        sess.deletions = 0;
        sess.json_data =
            "{\n"
            "  \"session_id\": \"" + sessionId + "\",\n"
            "  \"agent\": \"opencode\",\n"
            "  \"model\": \"test-model\",\n"
            "  \"author\": \"Test User <test@test.com>\",\n"
            "  \"ts_start\": 1,\n"
            "  \"ts_end\": 2,\n"
            "  \"additions\": " + std::to_string(additions) + ",\n"
            "  \"deletions\": 0,\n"
            "  \"entries\": [\n"
            "    {\"file_path\": \"" + filePath + "\", \"ranges\": \"" + ranges + "\"}\n"
            "  ]\n"
            "}\n";
        sess.committed = false;
        ASSERT_GT(db->saveSession(sess), 0);
    }

    std::string readSession(const std::string& sessionId) {
        auto* db = ghost::persist::getRepoDb(path);
        if (!db) return "";
        for (const auto& session : db->loadSessions(true)) {
            if (session.session_id == sessionId) return session.json_data;
        }
        return "";
    }

    bool sessionExists(const std::string& sessionId) {
        return !readSession(sessionId).empty();
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

TEST(PostCommitIntegration, ClipsSessionRangesToCommittedAddedLines) {
    TempGitRepo repo;

    repo.writeFile("src/app.txt", "one\ntwo\nthree\nfour\nfive\n");
    repo.addAndCommit("Initial commit");

    repo.writeSession("sess_clip", "src/app.txt", "1-20", 20);
    repo.writeFile("src/app.txt", "one\ntwo\nthree\nfour\nfive\nsix\nseven\n");
    repo.addAndCommit("AI append");

    int rc = 0;
    runCapture("\"" + ghostBin() + "\" post-commit", repo.path, &rc);
    EXPECT_EQ(rc, 0);

    std::string note = runCapture("git notes --ref=refs/notes/ghost show HEAD", repo.path, &rc);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(note.find("src/app.txt"), std::string::npos);
    EXPECT_NE(note.find("sess_clip 6-7"), std::string::npos);
    EXPECT_EQ(note.find("sess_clip 1-20"), std::string::npos);
    EXPECT_NE(note.find("\"additions\": 2"), std::string::npos);

    std::string pending = repo.readSession("sess_clip");
    EXPECT_NE(pending.find("\"ranges\": \"1-5,8-20\""), std::string::npos);
    EXPECT_NE(pending.find("\"additions\": 18"), std::string::npos);
}

TEST(PostCommitIntegration, SkipsSessionRangesOutsideCommittedAddedLines) {
    TempGitRepo repo;

    repo.writeFile("src/app.txt", "one\ntwo\nthree\nfour\nfive\n");
    repo.addAndCommit("Initial commit");

    repo.writeSession("sess_miss", "src/app.txt", "1-3", 3);
    repo.writeFile("src/app.txt", "one\ntwo\nthree\nfour\nfive\nsix\nseven\n");
    repo.addAndCommit("Human append");

    int rc = 0;
    runCapture("\"" + ghostBin() + "\" post-commit", repo.path, &rc);
    EXPECT_EQ(rc, 0);

    std::string note = runCapture("git notes --ref=refs/notes/ghost show HEAD", repo.path, &rc);
    EXPECT_NE(rc, 0);
    EXPECT_TRUE(note.find("No note found") != std::string::npos || note.find("error") != std::string::npos);
    EXPECT_TRUE(repo.sessionExists("sess_miss"));
    EXPECT_NE(repo.readSession("sess_miss").find("\"ranges\": \"1-3\""), std::string::npos);
}

TEST(PostCommitIntegration, SkipsMalformedSessionRangeWithoutCrashing) {
    TempGitRepo repo;

    repo.writeFile("src/app.txt", "one\ntwo\n");
    repo.addAndCommit("Initial commit");

    repo.writeSession("sess_bad", "src/app.txt", "bad-range", 99);
    repo.writeFile("src/app.txt", "one\ntwo\nthree\n");
    repo.addAndCommit("Append");

    int rc = 0;
    runCapture("\"" + ghostBin() + "\" post-commit", repo.path, &rc);
    EXPECT_EQ(rc, 0);

    std::string note = runCapture("git notes --ref=refs/notes/ghost show HEAD", repo.path, &rc);
    EXPECT_NE(rc, 0);
    EXPECT_TRUE(note.find("No note found") != std::string::npos || note.find("error") != std::string::npos);
}

TEST(PostCommitIntegration, DeduplicatesIdenticalCapturedSessions) {
    TempGitRepo repo;

    repo.writeFile("src/app.txt", "one\ntwo\n");
    repo.addAndCommit("Initial commit");

    repo.writeSession("sess_dup1", "src/app.txt", "3", 1);
    repo.writeSession("sess_dup2", "src/app.txt", "3", 1);
    repo.writeFile("src/app.txt", "one\ntwo\nthree\n");
    repo.addAndCommit("Append");

    int rc = 0;
    runCapture("\"" + ghostBin() + "\" post-commit", repo.path, &rc);
    EXPECT_EQ(rc, 0);

    std::string note = runCapture("git notes --ref=refs/notes/ghost show HEAD", repo.path, &rc);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(note.find("sess_dup1 3"), std::string::npos);
    EXPECT_EQ(note.find("sess_dup2"), std::string::npos);
    EXPECT_FALSE(repo.sessionExists("sess_dup1"));
    EXPECT_FALSE(repo.sessionExists("sess_dup2"));
}

TEST(PostCommitIntegration, CarriesUnconsumedSessionIntoLaterCommit) {
    TempGitRepo repo;

    repo.writeFile("src/app.txt", "one\ntwo\nthree\nfour\nfive\n");
    repo.addAndCommit("Initial commit");

    repo.writeSession("sess_carry", "src/app.txt", "6-7", 2);

    repo.writeFile("human.txt", "not ai\n");
    repo.addAndCommit("Human-only commit");

    int rc = 0;
    runCapture("\"" + ghostBin() + "\" post-commit", repo.path, &rc);
    EXPECT_EQ(rc, 0);
    EXPECT_TRUE(repo.sessionExists("sess_carry"));

    repo.writeFile("src/app.txt", "one\ntwo\nthree\nfour\nfive\nsix\nseven\n");
    repo.addAndCommit("Commit carried AI edit");

    runCapture("\"" + ghostBin() + "\" post-commit", repo.path, &rc);
    EXPECT_EQ(rc, 0);

    std::string note = runCapture("git notes --ref=refs/notes/ghost show HEAD", repo.path, &rc);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(note.find("sess_carry 6-7"), std::string::npos);
    EXPECT_FALSE(repo.sessionExists("sess_carry"));
}
