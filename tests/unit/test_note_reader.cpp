#include <gtest/gtest.h>
#include "reader.hpp"
#include "gitai_reader.hpp"
#include "writer.hpp"
#include <ctime>

using ghost::note::NoteReader;
using ghost::note::NoteWriter;
using ghost::note::AuthorshipEntry;
using ghost::note::Session;
using ghost::note::LineRangeSet;
using ghost::note::GitAiReader;

TEST(NoteReader, ValidNote) {
    std::string note = 
        "src/main.cpp\n"
        "  sess_abc 5-12,18\n"
        "---\n"
        "{\n"
        "  \"schema\": \"ghost/1.0.0\",\n"
        "  \"commit\": \"abc123\",\n"
        "  \"sessions\": {\n"
        "    \"sess_abc\": {\n"
        "      \"session_id\": \"sess_abc\",\n"
        "      \"agent\": \"opencode\",\n"
        "      \"model\": \"claude-sonnet\",\n"
        "      \"author\": \"Test User <test@test.com>\",\n"
        "      \"ts_start\": 1710000000,\n"
        "      \"ts_end\": 1710000100,\n"
        "      \"additions\": 10,\n"
        "      \"deletions\": 2\n"
        "    }\n"
        "  }\n"
        "}\n";

    auto result = NoteReader::parse(note);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.error, "");
    EXPECT_EQ(result.commit_sha, "abc123");
    EXPECT_EQ(result.entries.size(), 1);
    EXPECT_EQ(result.sessions.size(), 1);

    EXPECT_EQ(result.entries[0].file_path, "src/main.cpp");
    EXPECT_EQ(result.entries[0].session_id, "sess_abc");
    EXPECT_EQ(result.entries[0].ranges.toString(), "5-12,18");

    auto it = result.sessions.find("sess_abc");
    EXPECT_NE(it, result.sessions.end());
    EXPECT_EQ(it->second.agent, "opencode");
    EXPECT_EQ(it->second.model, "claude-sonnet");
    EXPECT_EQ(it->second.additions, 10);
    EXPECT_EQ(it->second.deletions, 2);
}

TEST(NoteReader, EmptyNote) {
    auto result = NoteReader::parse("");
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error, "");
}

TEST(NoteReader, MissingSeparator) {
    std::string note = 
        "src/main.cpp\n"
        "  sess_abc 5-12,18\n";
    // No --- separator
    
    auto result = NoteReader::parse(note);
    // Should either fail gracefully or parse what it can
    // Behavior is implementation-defined, just ensure no crash
    EXPECT_NO_THROW(NoteReader::parse(note));
}

TEST(NoteReader, MultipleEntries) {
    std::string note = 
        "src/main.cpp\n"
        "  sess_abc 5-12,18\n"
        "src/lib.cpp\n"
        "  sess_abc 1-10\n"
        "---\n"
        "{\n"
        "  \"schema\": \"ghost/1.0.0\",\n"
        "  \"commit\": \"multi123\",\n"
        "  \"sessions\": {\n"
        "    \"sess_abc\": {\n"
        "      \"session_id\": \"sess_abc\",\n"
        "      \"agent\": \"opencode\",\n"
        "      \"model\": \"gpt-4\",\n"
        "      \"author\": \"Alice\",\n"
        "      \"ts_start\": 0,\n"
        "      \"ts_end\": 0,\n"
        "      \"additions\": 20,\n"
        "      \"deletions\": 5\n"
        "    }\n"
        "  }\n"
        "}\n";

    auto result = NoteReader::parse(note);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.entries.size(), 2);
    
    EXPECT_EQ(result.entries[0].file_path, "src/main.cpp");
    EXPECT_EQ(result.entries[1].file_path, "src/lib.cpp");
}

TEST(NoteReader, MultipleSessions) {
    std::string note = 
        "src/main.cpp\n"
        "  sess_abc 5-10\n"
        "src/test.cpp\n"
        "  sess_def 1-5\n"
        "---\n"
        "{\n"
        "  \"schema\": \"ghost/1.0.0\",\n"
        "  \"commit\": \"multi2\",\n"
        "  \"sessions\": {\n"
        "    \"sess_abc\": {\n"
        "      \"session_id\": \"sess_abc\",\n"
        "      \"agent\": \"opencode\",\n"
        "      \"model\": \"gpt-4\",\n"
        "      \"author\": \"Alice\",\n"
        "      \"ts_start\": 0,\n"
        "      \"ts_end\": 0,\n"
        "      \"additions\": 10,\n"
        "      \"deletions\": 0\n"
        "    },\n"
        "    \"sess_def\": {\n"
        "      \"session_id\": \"sess_def\",\n"
        "      \"agent\": \"cursor\",\n"
        "      \"model\": \"claude\",\n"
        "      \"author\": \"Bob\",\n"
        "      \"ts_start\": 0,\n"
        "      \"ts_end\": 0,\n"
        "      \"additions\": 5,\n"
        "      \"deletions\": 0\n"
        "    }\n"
        "  }\n"
        "}\n";

    auto result = NoteReader::parse(note);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.entries.size(), 2);
    EXPECT_EQ(result.sessions.size(), 2);

    EXPECT_EQ(result.entries[0].session_id, "sess_abc");
    EXPECT_EQ(result.entries[1].session_id, "sess_def");

    auto it = result.sessions.find("sess_def");
    EXPECT_NE(it, result.sessions.end());
    EXPECT_EQ(it->second.agent, "cursor");
    EXPECT_EQ(it->second.model, "claude");
}

TEST(NoteReader, RoundTripWithWriter) {
    std::vector<AuthorshipEntry> entries;
    std::map<std::string, Session> sessions;

    AuthorshipEntry entry;
    entry.file_path = "src/app.cpp";
    entry.session_id = "sess_xyz";
    entry.ranges = LineRangeSet::parse("1-100,200");
    entries.push_back(entry);

    Session sess;
    sess.session_id = "sess_xyz";
    sess.agent = "copilot";
    sess.model = "gpt-4o";
    sess.author = "Dev <dev@test.com>";
    sess.ts_start = 1710000000;
    sess.ts_end = 1710001000;
    sess.additions = 101;
    sess.deletions = 0;
    sessions["sess_xyz"] = sess;

    std::string written = NoteWriter::write(entries, sessions, "round123");
    auto result = NoteReader::parse(written);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.commit_sha, "round123");
    EXPECT_EQ(result.entries.size(), 1);
    EXPECT_EQ(result.sessions.size(), 1);
    EXPECT_EQ(result.entries[0].ranges.toString(), "1-100,200");
}

TEST(NoteReader, InvalidJson) {
    std::string note = 
        "src/main.cpp\n"
        "  sess_abc 5-10\n"
        "---\n"
        "{ invalid json }\n";

    auto result = NoteReader::parse(note);
    // Should handle gracefully
    EXPECT_NO_THROW(NoteReader::parse(note));
}

TEST(GitAiReader, ParsesAuthorshipV3Note) {
    std::string note =
        "hooks/post_clone_hook.rs\n"
        "  a1b2c3d4e5f6a7b8 6-8\n"
        "  c9d0e1f2a3b4c5d6 16,21,25\n"
        "---\n"
        "{\n"
        "  \"schema_version\": \"authorship/3.0.0\",\n"
        "  \"git_ai_version\": \"0.1.4\",\n"
        "  \"base_commit_sha\": \"f4a8b2c\",\n"
        "  \"prompts\": {\n"
        "    \"a1b2c3d4e5f6a7b8\": {\n"
        "      \"agent_id\": {\"tool\": \"opencode\", \"model\": \"qwen3-coder\"},\n"
        "      \"human_author\": \"Alice Person <alice@example.com>\",\n"
        "      \"total_additions\": 8,\n"
        "      \"total_deletions\": 0\n"
        "    },\n"
        "    \"c9d0e1f2a3b4c5d6\": {\n"
        "      \"agent_id\": {\"tool\": \"cursor\", \"model\": \"sonnet-4.5\"},\n"
        "      \"human_author\": \"Jeff Coder <jeff@example.com>\",\n"
        "      \"total_additions\": 5,\n"
        "      \"total_deletions\": 2\n"
        "    }\n"
        "  }\n"
        "}\n";

    auto result = GitAiReader::parse(note);

    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.entries.size(), 2);
    EXPECT_EQ(result.entries[0].file_path, "hooks/post_clone_hook.rs");
    EXPECT_EQ(result.entries[0].session_id, "a1b2c3d4e5f6a7b8");
    EXPECT_EQ(result.entries[0].ranges.toString(), "6-8");
    EXPECT_EQ(result.entries[1].ranges.toString(), "16,21,25");
    ASSERT_EQ(result.sessions.size(), 2);
    EXPECT_EQ(result.sessions["a1b2c3d4e5f6a7b8"].agent, "opencode");
    EXPECT_EQ(result.sessions["a1b2c3d4e5f6a7b8"].model, "qwen3-coder");
    EXPECT_EQ(result.sessions["a1b2c3d4e5f6a7b8"].author, "Alice Person <alice@example.com>");
    EXPECT_EQ(result.sessions["a1b2c3d4e5f6a7b8"].additions, 8);
    EXPECT_EQ(result.sessions["c9d0e1f2a3b4c5d6"].deletions, 2);
}
