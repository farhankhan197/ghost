#include <gtest/gtest.h>
#include "path.hpp"
#include <filesystem>

namespace fs = std::filesystem;

using ghost::git::Path;

TEST(GitPath, NormalizesRelativePath) {
    auto result = Path::normalizeRepoPath("./src\\main.cpp", "");

    EXPECT_TRUE(result.inside_repo);
    EXPECT_EQ(result.path, "src/main.cpp");
}

TEST(GitPath, RejectsParentTraversal) {
    auto result = Path::normalizeRepoPath("../outside.cpp", "");

    EXPECT_FALSE(result.inside_repo);
    EXPECT_TRUE(result.path.empty());
}

TEST(GitPath, AllowsDotsInsideFileName) {
    auto result = Path::normalizeRepoPath("src/foo..bar.cpp", "");

    EXPECT_TRUE(result.inside_repo);
    EXPECT_EQ(result.path, "src/foo..bar.cpp");
}

TEST(GitPath, NormalizesAbsolutePathUnderRepo) {
    fs::path repo = fs::temp_directory_path() / "ghost-path-test";
    fs::path file = repo / "src" / "main.cpp";

    auto result = Path::normalizeRepoPath(file.string(), repo.string());

    EXPECT_TRUE(result.inside_repo);
    EXPECT_EQ(result.path, "src/main.cpp");
}

TEST(GitPath, RejectsAbsolutePathOutsideRepo) {
    fs::path repo = fs::temp_directory_path() / "ghost-path-test";
    fs::path file = fs::temp_directory_path() / "outside.cpp";

    auto result = Path::normalizeRepoPath(file.string(), repo.string());

    EXPECT_FALSE(result.inside_repo);
    EXPECT_TRUE(result.path.empty());
}
