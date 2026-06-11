#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <memory>
#include <string>
#include <chrono>
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

class BlameRepo {
public:
    std::string path;

    BlameRepo() {
        auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(rand());
        path = (fs::temp_directory_path() / ("ghost-blame-test-" + suffix)).string();
        fs::create_directories(path);
        runCapture("git init", path);
        runCapture("git config user.name \"Blame User\"", path);
        runCapture("git config user.email \"blame@example.com\"", path);
    }

    ~BlameRepo() {
        std::error_code ec;
        ghost::persist::closeRepoDb(path);
        fs::remove_all(path, ec);
    }

    void write(const std::string& rel, const std::string& content) {
        fs::path p = fs::path(path) / rel;
        fs::create_directories(p.parent_path());
        std::ofstream out(p);
        out << content;
    }

    void writeSession(const std::string& filePath, const std::string& ranges) {
        fs::create_directories(fs::path(path) / ".git" / "ghost");
        auto* db = ghost::persist::getRepoDb(path);
        ASSERT_NE(db, nullptr);

        ghost::persist::Session sess;
        sess.session_id = "sess_blame";
        sess.agent = "opencode";
        sess.model = "test-model";
        sess.author = "Blame User <blame@example.com>";
        sess.ts_start = 1;
        sess.ts_end = 2;
        sess.additions = 2;
        sess.deletions = 0;
        sess.json_data = "{\"session_id\":\"sess_blame\",\"agent\":\"opencode\",\"model\":\"test-model\",\"author\":\"Blame User <blame@example.com>\",\"ts_start\":1,\"ts_end\":2,\"additions\":2,\"deletions\":0,\"entries\":[{\"file_path\":\"" + filePath + "\",\"ranges\":\"" + ranges + "\"}]}";
        sess.committed = false;
        ASSERT_GT(db->saveSession(sess), 0);
    }
};

TEST(BlameCli, PreservesAttributionAcrossRename) {
    BlameRepo repo;
    repo.writeSession("old.txt", "1-2");
    repo.write("old.txt", "one\ntwo\n");
    runCapture("git add old.txt", repo.path);
    runCapture("git commit -m \"AI file\"", repo.path);

    int rc = 0;
    runCapture("\"" + ghostBin() + "\" post-commit", repo.path, &rc);
    EXPECT_EQ(rc, 0);

    runCapture("git mv old.txt new.txt", repo.path);
    runCapture("git commit -m \"Rename file\"", repo.path);

    std::string out = runCapture("\"" + ghostBin() + "\" blame new.txt --json", repo.path, &rc);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("\"total_lines\": 2"), std::string::npos);
    EXPECT_NE(out.find("\"ai_lines\": 2"), std::string::npos);
    EXPECT_NE(out.find("\"agent\": \"opencode\""), std::string::npos);
}

TEST(BlameCli, MatchesRangesAfterFirstBlameHeader) {
    BlameRepo repo;
    repo.writeSession("app.txt", "2-3");
    repo.write("app.txt", "one\ntwo\nthree\n");
    runCapture("git add app.txt", repo.path);
    runCapture("git commit -m \"AI partial file\"", repo.path);

    int rc = 0;
    runCapture("\"" + ghostBin() + "\" post-commit", repo.path, &rc);
    EXPECT_EQ(rc, 0);

    std::string out = runCapture("\"" + ghostBin() + "\" blame app.txt --json", repo.path, &rc);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("\"total_lines\": 3"), std::string::npos);
    EXPECT_NE(out.find("\"ai_lines\": 2"), std::string::npos);
    EXPECT_NE(out.find("\"line\": 1, \"commit\""), std::string::npos);
    EXPECT_NE(out.find("\"line\": 2, \"commit\""), std::string::npos);
    EXPECT_NE(out.find("\"line\": 3, \"commit\""), std::string::npos);

    out = runCapture("\"" + ghostBin() + "\" blame app.txt", repo.path, &rc);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("app.txt"), std::string::npos);
    EXPECT_NE(out.find("2 / 3 AI lines"), std::string::npos);
    EXPECT_NE(out.find("Range"), std::string::npos);
    EXPECT_NE(out.find("2-3"), std::string::npos);
    EXPECT_NE(out.find("opencode/test-model"), std::string::npos);
    EXPECT_EQ(out.find("\"line\""), std::string::npos);

    out = runCapture("\"" + ghostBin() + "\" --verbose blame app.txt", repo.path, &rc);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("Line"), std::string::npos);
    EXPECT_NE(out.find("Commit"), std::string::npos);
    EXPECT_NE(out.find("opencode/test-model"), std::string::npos);
}
