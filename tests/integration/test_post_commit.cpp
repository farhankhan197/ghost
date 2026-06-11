#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <string>
#include <chrono>
#include <sstream>
#include <vector>
#include "test_command.hpp"
#include "persist/db.hpp"

namespace fs = std::filesystem;

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
        
        ghost::test::git(path, {"init"});
        ghost::test::git(path, {"config", "user.name", "Test User"});
        ghost::test::git(path, {"config", "user.email", "test@test.com"});
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
        ghost::test::git(path, {"add", "-A"});
        ghost::test::git(path, {"commit", "-m", msg});
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

    auto postCommit = ghost::test::run(repo.path, ghostBin(), {"post-commit"});
    EXPECT_EQ(postCommit.exitCode, 0) << ghost::test::output(postCommit);

    auto noteResult = ghost::test::git(repo.path, {"notes", "--ref=refs/notes/ghost", "show", "HEAD"});
    std::string note = ghost::test::output(noteResult);
    EXPECT_EQ(noteResult.exitCode, 0) << note;
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

    auto postCommit = ghost::test::run(repo.path, ghostBin(), {"post-commit"});
    EXPECT_EQ(postCommit.exitCode, 0) << ghost::test::output(postCommit);

    auto noteResult = ghost::test::git(repo.path, {"notes", "--ref=refs/notes/ghost", "show", "HEAD"});
    std::string note = ghost::test::output(noteResult);
    EXPECT_NE(noteResult.exitCode, 0);
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

    auto postCommit = ghost::test::run(repo.path, ghostBin(), {"post-commit"});
    EXPECT_EQ(postCommit.exitCode, 0) << ghost::test::output(postCommit);

    auto noteResult = ghost::test::git(repo.path, {"notes", "--ref=refs/notes/ghost", "show", "HEAD"});
    std::string note = ghost::test::output(noteResult);
    EXPECT_NE(noteResult.exitCode, 0);
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

    auto postCommit = ghost::test::run(repo.path, ghostBin(), {"post-commit"});
    EXPECT_EQ(postCommit.exitCode, 0) << ghost::test::output(postCommit);

    auto noteResult = ghost::test::git(repo.path, {"notes", "--ref=refs/notes/ghost", "show", "HEAD"});
    std::string note = ghost::test::output(noteResult);
    EXPECT_EQ(noteResult.exitCode, 0) << note;
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

    auto firstPostCommit = ghost::test::run(repo.path, ghostBin(), {"post-commit"});
    EXPECT_EQ(firstPostCommit.exitCode, 0) << ghost::test::output(firstPostCommit);
    EXPECT_TRUE(repo.sessionExists("sess_carry"));

    repo.writeFile("src/app.txt", "one\ntwo\nthree\nfour\nfive\nsix\nseven\n");
    repo.addAndCommit("Commit carried AI edit");

    auto secondPostCommit = ghost::test::run(repo.path, ghostBin(), {"post-commit"});
    EXPECT_EQ(secondPostCommit.exitCode, 0) << ghost::test::output(secondPostCommit);

    auto noteResult = ghost::test::git(repo.path, {"notes", "--ref=refs/notes/ghost", "show", "HEAD"});
    std::string note = ghost::test::output(noteResult);
    EXPECT_EQ(noteResult.exitCode, 0) << note;
    EXPECT_NE(note.find("sess_carry 6-7"), std::string::npos);
    EXPECT_FALSE(repo.sessionExists("sess_carry"));
}

TEST(PostCommitIntegration, RerunPreservesExistingVerifiedSessionCount) {
    TempGitRepo repo;

    repo.writeFile("src/app.txt", "one\ntwo\n");
    repo.addAndCommit("Initial commit");

    repo.writeSession("sess_once", "src/app.txt", "3", 1);
    repo.writeFile("src/app.txt", "one\ntwo\nthree\n");
    repo.addAndCommit("AI append");

    auto firstPostCommit = ghost::test::run(repo.path, ghostBin(), {"post-commit"});
    EXPECT_EQ(firstPostCommit.exitCode, 0) << ghost::test::output(firstPostCommit);

    auto firstGhostNoteResult = ghost::test::git(repo.path, {"notes", "--ref=refs/notes/ghost", "show", "HEAD"});
    std::string firstGhostNote = ghost::test::output(firstGhostNoteResult);
    EXPECT_EQ(firstGhostNoteResult.exitCode, 0) << firstGhostNote;
    EXPECT_NE(firstGhostNote.find("sess_once 3"), std::string::npos);

    auto firstVerifiedResult = ghost::test::git(repo.path, {"notes", "--ref=refs/notes/ghost-verified", "show", "HEAD"});
    std::string firstVerifiedNote = ghost::test::output(firstVerifiedResult);
    EXPECT_EQ(firstVerifiedResult.exitCode, 0) << firstVerifiedNote;
    EXPECT_NE(firstVerifiedNote.find("\"sessions\": 1"), std::string::npos);
    EXPECT_FALSE(repo.sessionExists("sess_once"));

    auto rerun = ghost::test::run(repo.path, ghostBin(), {"post-commit"});
    EXPECT_EQ(rerun.exitCode, 0) << ghost::test::output(rerun);

    auto secondGhostNoteResult = ghost::test::git(repo.path, {"notes", "--ref=refs/notes/ghost", "show", "HEAD"});
    std::string secondGhostNote = ghost::test::output(secondGhostNoteResult);
    EXPECT_EQ(secondGhostNoteResult.exitCode, 0) << secondGhostNote;
    EXPECT_EQ(secondGhostNote, firstGhostNote);

    auto secondVerifiedResult = ghost::test::git(repo.path, {"notes", "--ref=refs/notes/ghost-verified", "show", "HEAD"});
    std::string secondVerifiedNote = ghost::test::output(secondVerifiedResult);
    EXPECT_EQ(secondVerifiedResult.exitCode, 0) << secondVerifiedNote;
    EXPECT_NE(secondVerifiedNote.find("\"sessions\": 1"), std::string::npos);
    EXPECT_EQ(secondVerifiedNote.find("\"sessions\": 0"), std::string::npos);
}
