#include <gtest/gtest.h>
#include <filesystem>
#include "checkpoint/hook_event.hpp"
#include "checkpoint/session_json.hpp"

TEST(HookEvent, ParsesCodexStylePayload) {
    std::string payload = R"JSON({
      "cwd": "C:/repo",
      "tool": "apply_patch",
      "args": {
        "file_path": "src/app.cpp"
      },
      "model": "openai/gpt-5.1-codex"
    })JSON";

    auto event = ghost::checkpoint::HookEvent::parse(payload);
    EXPECT_TRUE(event.valid_json);
    EXPECT_EQ(event.cwd, "C:/repo");
    EXPECT_EQ(event.file_path, "src/app.cpp");
    EXPECT_EQ(event.tool, "apply_patch");
    EXPECT_EQ(event.model, "gpt-5.1-codex");
    EXPECT_TRUE(event.isEditTool());
}

TEST(HookEvent, HandlesNestedOpenCodeStylePayloadAndMalformedJson) {
    std::string payload = R"JSON({
      "input": {
        "name": "edit",
        "args": {
          "path": "src/space file.cpp",
          "modelId": "qwen/qwen3.6-plus-free"
        }
      }
    })JSON";

    auto event = ghost::checkpoint::HookEvent::parse(payload);
    EXPECT_TRUE(event.valid_json);
    EXPECT_EQ(event.file_path, "src/space file.cpp");
    EXPECT_EQ(event.model, "qwen3.6-plus-free");
    EXPECT_EQ(event.tool, "edit");
    EXPECT_TRUE(event.isEditTool());

    auto bad = ghost::checkpoint::HookEvent::parse("{not json");
    EXPECT_FALSE(bad.valid_json);
    EXPECT_TRUE(bad.file_path.empty());
}

TEST(HookEvent, ClassifiesCodexAndMcpEditToolsWithoutMatchingShellTools) {
    auto patch = ghost::checkpoint::HookEvent::parse(R"JSON({"tool": "functions.apply_patch"})JSON");
    EXPECT_TRUE(patch.valid_json);
    EXPECT_TRUE(patch.isEditTool());

    auto write = ghost::checkpoint::HookEvent::parse(R"JSON({"tool": "mcp__filesystem__write_file"})JSON");
    EXPECT_TRUE(write.valid_json);
    EXPECT_TRUE(write.isEditTool());

    auto shell = ghost::checkpoint::HookEvent::parse(R"JSON({"tool": "Bash", "cwd": "C:/repo"})JSON");
    EXPECT_TRUE(shell.valid_json);
    EXPECT_FALSE(shell.isEditTool());
}

TEST(SessionJson, RoundTripsEscapedPathsAndMultiEntryRanges) {
    ghost::checkpoint::CapturedSession session;
    session.session_id = "sess_quote";
    session.agent = "opencode";
    session.model = "deepseek";
    session.author = "A User <a@example.com>";
    session.ts_start = 10;
    session.ts_end = 20;
    session.deletions = 2;
    session.entries = {
        {"src/space file.cpp", "1-2"},
        {"src/quote\"file.cpp", "5"}
    };

    std::string json = ghost::checkpoint::SessionJson::write(session);
    auto parsed = ghost::checkpoint::SessionJson::parse(json);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->session_id, session.session_id);
    EXPECT_EQ(parsed->agent, session.agent);
    EXPECT_EQ(parsed->model, session.model);
    EXPECT_EQ(parsed->entries.size(), 2);
    EXPECT_EQ(parsed->entries[1].file_path, "src/quote\"file.cpp");
    EXPECT_EQ(parsed->additions, 3);
}

TEST(SessionJson, ExtractsFilesRangesAndRejectsMalformedJson) {
    ghost::checkpoint::CapturedSession session;
    session.agent = "codex";
    session.model = "gpt";
    session.entries = {
        {"src\\app.cpp", "1-3"},
        {"src/app.cpp", "5"}
    };

    std::string json = ghost::checkpoint::SessionJson::write(session);
    auto files = ghost::checkpoint::SessionJson::files(json, "");
    ASSERT_EQ(files.size(), 1);
    EXPECT_EQ(files[0], "src/app.cpp");

    auto ranges = ghost::checkpoint::SessionJson::rangesForFile(json, "src/app.cpp", "");
    EXPECT_EQ(ranges.toString(), "1-3,5");

    EXPECT_FALSE(ghost::checkpoint::SessionJson::parse("{broken").has_value());
}
