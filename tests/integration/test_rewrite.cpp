#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <thread>
#include <chrono>
#include <memory>
#include <nlohmann/json.hpp>
#include "persist/db.hpp"
#include "rewrite/rewrite_log.hpp"
#include "rewrite/working_state.hpp"
#include "commit/note_index.hpp"

namespace fs = std::filesystem;

class RewriteIntegration : public ::testing::Test {
protected:
    std::string repoRoot;
    std::string dbPath;
    std::unique_ptr<ghost::persist::Database> db;

    void SetUp() override {
        repoRoot = fs::temp_directory_path().string() + "/ghost-rewrite-test-" + std::to_string(std::time(nullptr));
        fs::create_directories(repoRoot + "/.git/ghost");
    }

    void closeDb() {
        db.reset();
        ghost::persist::closeRepoDb(repoRoot);
    }

    void TearDown() override {
        closeDb();
        // On Windows, SQLite WAL files may briefly lock; retry removal
        for (int i = 0; i < 10; ++i) {
            std::error_code ec;
            fs::remove_all(repoRoot, ec);
            if (!ec) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
};

TEST_F(RewriteIntegration, DbCreateAndCheckpoint) {
    db = std::make_unique<ghost::persist::Database>(repoRoot + "/.git/ghost/ghost.db");
    EXPECT_TRUE(db->isOpen());

    ghost::persist::Checkpoint cp;
    cp.agent = "opencode";
    cp.model = "kimi";
    cp.target_file = "src/main.cpp";
    cp.snapshot_path = repoRoot + "/.git/ghost/snapshot/src/main.cpp";
    cp.ts_start = std::time(nullptr);
    cp.processed = false;

    int id = db->saveCheckpoint(cp);
    EXPECT_GT(id, 0);

    auto cps = db->loadCheckpoints(true);
    EXPECT_EQ(cps.size(), 1u);
    EXPECT_EQ(cps[0].agent, "opencode");
    EXPECT_EQ(cps[0].target_file, "src/main.cpp");

    db->markCheckpointProcessed(id);
    auto unprocessed = db->loadCheckpoints(true);
    EXPECT_TRUE(unprocessed.empty());
}

TEST_F(RewriteIntegration, RewriteLogAppendAndRead) {
    ghost::rewrite::RebaseCompleteEvent ev;
    ev.original_head = "abc123";
    ev.new_head = "def456";
    ev.is_interactive = false;
    ev.original_commits = {"aaa", "bbb"};
    ev.new_commits = {"ccc", "ddd"};

    int id = ghost::rewrite::RewriteLog::append(repoRoot, ghost::rewrite::RewriteEventType::RebaseComplete, ev.toJson());
    EXPECT_GT(id, 0);

    auto events = ghost::rewrite::RewriteLog::load(repoRoot, 10);
    EXPECT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].type, ghost::rewrite::RewriteEventType::RebaseComplete);

    ghost::persist::closeRepoDb(repoRoot);
}

TEST_F(RewriteIntegration, NoteIndexRoundTrip) {
    bool ok = ghost::commit::NoteIndex::update(repoRoot, "abc123def456", "refs/notes/ghost", true, 2);
    EXPECT_TRUE(ok);

    auto entry = ghost::commit::NoteIndex::get(repoRoot, "abc123def456");
    EXPECT_TRUE(entry.has_value());
    EXPECT_TRUE(entry->note_exists);
    EXPECT_EQ(entry->session_count, 2);
    EXPECT_EQ(entry->note_ref, "refs/notes/ghost");

    auto all = ghost::commit::NoteIndex::getAll(repoRoot);
    EXPECT_EQ(all.size(), 1u);

    ghost::persist::closeRepoDb(repoRoot);
}

TEST_F(RewriteIntegration, WorkingStateSaveRestore) {
    db = std::make_unique<ghost::persist::Database>(repoRoot + "/.git/ghost/ghost.db");
    ghost::persist::Session sess;
    sess.session_id = "sess_test123";
    sess.agent = "opencode";
    sess.model = "kimi";
    sess.author = "dev@example.com";
    sess.ts_start = 1000;
    sess.ts_end = 2000;
    sess.additions = 10;
    sess.deletions = 2;
    sess.json_data = "{\"test\":true}";
    sess.committed = false;
    db->saveSession(sess);

    // Verify session exists before save
    auto before = db->loadSessions(true);
    EXPECT_EQ(before.size(), 1u);
    closeDb();

    // Save working state (serializes sessions to working_state table)
    bool ok = ghost::rewrite::WorkingState::save(repoRoot, "stash");
    EXPECT_TRUE(ok);

    auto* stateDb = ghost::persist::getRepoDb(repoRoot);
    ASSERT_NE(stateDb, nullptr);
    auto rawState = stateDb->loadWorkingState("sessions_stash");
    ASSERT_TRUE(rawState.has_value());
    EXPECT_NO_THROW({
        auto parsed = nlohmann::json::parse(rawState.value());
        EXPECT_TRUE(parsed.is_object());
        EXPECT_TRUE(parsed.contains("sessions"));
    });
    ghost::persist::closeRepoDb(repoRoot);

    // Restore working state (should re-create sessions from working_state)
    ok = ghost::rewrite::WorkingState::restore(repoRoot, "stash");
    EXPECT_TRUE(ok);
    ghost::persist::closeRepoDb(repoRoot);

    // Reopen DB to verify restored session exists
    db = std::make_unique<ghost::persist::Database>(repoRoot + "/.git/ghost/ghost.db");
    auto restored = db->loadSessions(true);
    // Should have the original + restored session (2 total)
    EXPECT_GE(restored.size(), 1u);
    bool found = false;
    for (const auto& s : restored) {
        if (s.session_id == "sess_test123") found = true;
    }
    EXPECT_TRUE(found);
}
