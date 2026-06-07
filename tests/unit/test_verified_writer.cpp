#include <gtest/gtest.h>
#include "verified_writer.hpp"
#include "verified_reader.hpp"
#include <ctime>

using ghost::note::VerifiedWriter;
using ghost::note::VerifiedReader;
using ghost::note::VerifiedNote;

TEST(VerifiedWriter, BasicStructure) {
    VerifiedNote note;
    note.schema = "ghost-verified/1.0.0";
    note.ghost_version = GHOST_VERSION;
    note.commit = "abc123";
    note.ts = 1710000000;
    note.author = "Test User <test@test.com>";
    note.sessions = 2;

    std::string output = VerifiedWriter::write(note);

    EXPECT_NE(output.find("\"schema\": \"ghost-verified/1.0.0\""), std::string::npos);
    EXPECT_NE(output.find(std::string("\"ghost_version\": \"") + GHOST_VERSION + "\""), std::string::npos);
    EXPECT_NE(output.find("\"commit\": \"abc123\""), std::string::npos);
    EXPECT_NE(output.find("\"ts\": 1710000000"), std::string::npos);
    EXPECT_NE(output.find("\"author\": \"Test User <test@test.com>\""), std::string::npos);
    EXPECT_NE(output.find("\"sessions\": 2"), std::string::npos);
}

TEST(VerifiedWriter, ZeroSessions) {
    VerifiedNote note;
    note.schema = "ghost-verified/1.0.0";
    note.ghost_version = GHOST_VERSION;
    note.commit = "zero123";
    note.ts = 0;
    note.author = "Anonymous";
    note.sessions = 0;

    std::string output = VerifiedWriter::write(note);
    EXPECT_NE(output.find("\"sessions\": 0"), std::string::npos);
}

TEST(VerifiedWriter, RoundTrip) {
    VerifiedNote note;
    note.schema = "ghost-verified/1.0.0";
    note.ghost_version = GHOST_VERSION;
    note.commit = "round123";
    note.ts = 1710000500;
    note.author = "Alice <alice@test.com>";
    note.sessions = 5;

    std::string written = VerifiedWriter::write(note);
    auto result = VerifiedReader::parse(written);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.error, "");
    EXPECT_EQ(result.note.schema, "ghost-verified/1.0.0");
    EXPECT_EQ(result.note.commit, "round123");
    EXPECT_EQ(result.note.ts, 1710000500);
    EXPECT_EQ(result.note.author, "Alice <alice@test.com>");
    EXPECT_EQ(result.note.sessions, 5);
}

TEST(VerifiedReader, ValidJson) {
    std::string json = 
        "{\n"
        "  \"schema\": \"ghost-verified/1.0.0\",\n"
        "  \"ghost_version\": \"1.0.0\",\n"
        "  \"commit\": \"abc123\",\n"
        "  \"ts\": 1710000000,\n"
        "  \"author\": \"Test User\",\n"
        "  \"sessions\": 3\n"
        "}\n";

    auto result = VerifiedReader::parse(json);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.note.commit, "abc123");
    EXPECT_EQ(result.note.sessions, 3);
}

TEST(VerifiedReader, InvalidJson) {
    std::string json = "{ invalid json }";

    auto result = VerifiedReader::parse(json);
    // Should handle gracefully without crashing
    EXPECT_NO_THROW(VerifiedReader::parse(json));
}

TEST(VerifiedReader, EmptyJson) {
    std::string json = "";

    auto result = VerifiedReader::parse(json);
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error, "");
}
