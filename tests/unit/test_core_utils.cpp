#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include "cli/args.hpp"
#include "cli/commands.hpp"
#include "config/ignore_matcher.hpp"
#include "util/files.hpp"
#include "util/text.hpp"

#include <set>

TEST(TextUtil, TrimLowerSplitAndParse) {
    EXPECT_EQ(ghost::util::Text::trim(" \t Hello \r\n"), "Hello");
    EXPECT_EQ(ghost::util::Text::lower("AbC-123"), "abc-123");

    auto lines = ghost::util::Text::splitLines("a\nb\r\nc");
    ASSERT_EQ(lines.size(), 3);
    EXPECT_EQ(lines[0], "a");
    EXPECT_EQ(lines[1], "b");
    EXPECT_EQ(lines[2], "c");

    ASSERT_TRUE(ghost::util::Text::parseInt(" 42 ").has_value());
    EXPECT_EQ(*ghost::util::Text::parseInt(" 42 "), 42);
    EXPECT_FALSE(ghost::util::Text::parseInt("42x").has_value());
}

TEST(ArgsUtil, ReadsFlagsValuesAndPositionals) {
    const char* raw[] = {"ghost", "--verbose", "check", "--json", "--config-ref", "origin/main"};
    char** argv = const_cast<char**>(raw);
    ghost::cli::Args args(6, argv);

    EXPECT_TRUE(args.hasFlag("--verbose"));
    EXPECT_TRUE(args.hasAnyFlag({"--json", "-j"}));
    EXPECT_EQ(args.getValue("--config-ref"), "origin/main");
    EXPECT_EQ(args.getValue("--missing"), "");

    auto positionals = args.positional();
    ASSERT_GE(positionals.size(), 2);
    EXPECT_EQ(positionals[0], "check");
    EXPECT_EQ(positionals[1], "origin/main");
}

TEST(CommandRegistry, AliasesAreUniqueAndPredictable) {
    std::set<std::string> aliases;
    for (const auto& [name, info] : ghost::cli::CommandRegistry::getCommands()) {
        for (const auto& alias : info.aliases) {
            ASSERT_TRUE(aliases.insert(alias).second)
                << "duplicate alias " << alias << " while checking " << name;
        }
    }

    EXPECT_EQ(ghost::cli::CommandRegistry::resolveCommand("b"), "blame");
    EXPECT_EQ(ghost::cli::CommandRegistry::resolveCommand("ban"), "banish");
    EXPECT_EQ(ghost::cli::CommandRegistry::resolveCommand("st"), "status");
    EXPECT_EQ(ghost::cli::CommandRegistry::resolveCommand("stat"), "stats");
}

TEST(IgnoreMatcher, MatchesExistingGhostPatterns) {
    std::vector<std::string> patterns = {"node_modules/", "*.min.js", ".git/", "dist"};

    EXPECT_TRUE(ghost::config::IgnoreMatcher::matches("node_modules/pkg/index.js", patterns));
    EXPECT_TRUE(ghost::config::IgnoreMatcher::matches("src\\vendor\\node_modules\\pkg.js", patterns));
    EXPECT_TRUE(ghost::config::IgnoreMatcher::matches("public/app.min.js", patterns));
    EXPECT_TRUE(ghost::config::IgnoreMatcher::matches(".git/config", patterns));
    EXPECT_TRUE(ghost::config::IgnoreMatcher::matches("web/dist/app.js", patterns));
    EXPECT_FALSE(ghost::config::IgnoreMatcher::matches("src/main.cpp", patterns));
}

TEST(FilesUtil, WritesMissingTextAndHonorsForce) {
    auto dir = std::filesystem::temp_directory_path() / "ghost-files-util-test";
    auto file = dir / "nested" / "file.txt";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    ASSERT_TRUE(ghost::util::Files::writeTextIfMissing(file, "first", false));
    ASSERT_TRUE(ghost::util::Files::exists(file));
    ASSERT_TRUE(ghost::util::Files::writeTextIfMissing(file, "second", false));

    std::ifstream in(file);
    std::string value;
    std::getline(in, value);
    EXPECT_EQ(value, "first");

    ASSERT_TRUE(ghost::util::Files::writeTextIfMissing(file, "second", true));
    std::ifstream inForced(file);
    std::getline(inForced, value);
    EXPECT_EQ(value, "second");

    auto direct = dir / "direct.txt";
    ASSERT_TRUE(ghost::util::Files::writeText(direct, "hello\nworld"));
    EXPECT_EQ(ghost::util::Files::readText(direct), "hello\nworld");

    std::filesystem::remove_all(dir, ec);
}
