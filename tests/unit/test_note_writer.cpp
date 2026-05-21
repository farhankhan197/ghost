#include <gtest/gtest.h>
#include "writer.hpp"
#include "reader.hpp"
#include <ctime>

using ghost::note::NoteWriter;
using ghost::note::NoteReader;
using ghost::note::AuthorshipEntry;
using ghost::note::Session;
using ghost::note::LineRangeSet;

TEST(NoteWriter, BasicStructure) {
    std::vector<AuthorshipEntry> entries;
    std::map<std::string, Session> sessions;
    std::string commit_sha = "abc123";

    AuthorshipEntry entry;
    entry.file_path = "src/main.cpp";
    entry.session_id = "sess_abc";
    entry.ranges = LineRangeSet::parse("5-12,18");
    entries.push_back(entry);

    Session sess;
    sess.session_id = "sess_abc";
    sess.agent = "opencode";
    sess.model = "claude-sonnet";
    sess.author = "Test User <test@test.com>";
    sess.ts_start = 1710000000;
    sess.ts_end = 1710000100;
    sess.additions = 10;
    sess.deletions = 2;
    sessions["sess_abc"] = sess;

    std::string output = NoteWriter::write(entries, sessions, commit_sha);

    // Verify top section exists
    EXPECT_NE(output.find("src/main.cpp"), std::string::npos);
    EXPECT_NE(output.find("sess_abc"), std::string::npos);
    EXPECT_NE(output.find("5-12,18"), std::string::npos);

    // Verify separator exists
    EXPECT_NE(output.find("---"), std::string::npos);

    // Verify JSON section exists
    EXPECT_NE(output.find("\"schema\": \"ghost/1.0.0\""), std::string::npos);
    EXPECT_NE(output.find("\"commit\": \"abc123\""), std::string::npos);
    EXPECT_NE(output.find("\"agent\": \"opencode\""), std::string::npos);
    EXPECT_NE(output.find("\"model\": \"claude-sonnet\""), std::string::npos);
}

TEST(NoteWriter, RoundTrip) {
    std::vector<AuthorshipEntry> entries;
    std::map<std::string, Session> sessions;

    AuthorshipEntry entry1;
    entry1.file_path = "src/main.cpp";
    entry1.session_id = "sess_abc";
    entry1.ranges = LineRangeSet::parse("5-12,18,22-30");
    entries.push_back(entry1);

    AuthorshipEntry entry2;
    entry2.file_path = "src/lib.cpp";
    entry2.session_id = "sess_abc";
    entry2.ranges = LineRangeSet::parse("1-10");
    entries.push_back(entry2);

    Session sess;
    sess.session_id = "sess_abc";
    sess.agent = "opencode";
    sess.model = "gpt-4";
    sess.author = "Alice <alice@test.com>";
    sess.ts_start = 1710000000;
    sess.ts_end = 1710000100;
    sess.additions = 25;
    sess.deletions = 3;
    sessions["sess_abc"] = sess;

    std::string output = NoteWriter::write(entries, sessions, "commit123");
    auto result = NoteReader::parse(output);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.entries.size(), 2);
    EXPECT_EQ(result.sessions.size(), 1);
    
    // NoteReader may return entries in different order than written
    bool foundMain = false, foundLib = false;
    for (const auto& entry : result.entries) {
        if (entry.file_path == "src/main.cpp") {
            foundMain = true;
            EXPECT_EQ(entry.ranges.toString(), "5-12,18,22-30");
        }
        if (entry.file_path == "src/lib.cpp") {
            foundLib = true;
            EXPECT_EQ(entry.ranges.toString(), "1-10");
        }
    }
    EXPECT_TRUE(foundMain);
    EXPECT_TRUE(foundLib);
    
    auto it = result.sessions.find("sess_abc");
    EXPECT_NE(it, result.sessions.end());
    EXPECT_EQ(it->second.agent, "opencode");
    EXPECT_EQ(it->second.model, "gpt-4");
    EXPECT_EQ(it->second.additions, 25);
    EXPECT_EQ(it->second.deletions, 3);
}

TEST(NoteWriter, EmptyEntries) {
    std::vector<AuthorshipEntry> entries;
    std::map<std::string, Session> sessions;

    Session sess;
    sess.session_id = "sess_empty";
    sess.agent = "opencode";
    sess.model = "test";
    sess.author = "Test";
    sess.ts_start = 0;
    sess.ts_end = 0;
    sess.additions = 0;
    sess.deletions = 0;
    sessions["sess_empty"] = sess;

    std::string output = NoteWriter::write(entries, sessions, "empty123");
    
    // Should still produce valid output with schema and commit
    EXPECT_NE(output.find("\"schema\": \"ghost/1.0.0\""), std::string::npos);
    EXPECT_NE(output.find("\"commit\": \"empty123\""), std::string::npos);
}

TEST(NoteWriter, SpecialCharactersInPath) {
    std::vector<AuthorshipEntry> entries;
    std::map<std::string, Session> sessions;

    AuthorshipEntry entry;
    entry.file_path = "path/with spaces/file.cpp";
    entry.session_id = "sess_1";
    entry.ranges = LineRangeSet::parse("1-5");
    entries.push_back(entry);

    Session sess;
    sess.session_id = "sess_1";
    sess.agent = "opencode";
    sess.model = "test";
    sess.author = "Test";
    sess.ts_start = 0;
    sess.ts_end = 0;
    sess.additions = 5;
    sess.deletions = 0;
    sessions["sess_1"] = sess;

    std::string output = NoteWriter::write(entries, sessions, "special123");
    auto result = NoteReader::parse(output);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.entries.size(), 1);
    EXPECT_EQ(result.entries[0].file_path, "path/with spaces/file.cpp");
}
