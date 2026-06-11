#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "test_command.hpp"
#include "persist/db.hpp"

namespace fs = std::filesystem;

namespace {

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

class StatusRepo {
public:
    std::string path;

    StatusRepo() {
        auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(rand());
        path = (fs::temp_directory_path() / ("ghost-status-test-" + suffix)).string();
        fs::create_directories(path);
        ghost::test::git(path, {"init"});
        ghost::test::git(path, {"config", "user.name", "Status User"});
        ghost::test::git(path, {"config", "user.email", "status@example.com"});
        write("ghost.yml", "mode: transparent\nthreshold: 80\nrequired: true\non_exceed: warn\nignore:\n  - build/\n");
        write("src/app.txt", "one\n");
        ghost::test::git(path, {"add", "-A"});
        ghost::test::git(path, {"commit", "-m", "Initial"});
    }

    ~StatusRepo() {
        ghost::persist::closeRepoDb(path);
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    void write(const std::string& rel, const std::string& content) {
        fs::path p = fs::path(path) / rel;
        fs::create_directories(p.parent_path());
        std::ofstream out(p);
        out << content;
    }
};

}

TEST(StatusCli, DefaultShowsHealthWithoutDiagnostics) {
    StatusRepo repo;

    auto result = ghost::test::run(repo.path, ghostBin(), {"status"});
    std::string out = ghost::test::output(result);

    EXPECT_EQ(result.exitCode, 0) << out;
    EXPECT_NE(out.find("Ghost - status"), std::string::npos);
    EXPECT_NE(out.find("policy transparent"), std::string::npos);
    EXPECT_NE(out.find("Worktree"), std::string::npos);
    EXPECT_NE(out.find("Pending Attribution"), std::string::npos);
    EXPECT_NE(out.find("HEAD"), std::string::npos);
    EXPECT_EQ(out.find(repo.path), std::string::npos);
    EXPECT_EQ(out.find("post-commit"), std::string::npos);
    EXPECT_EQ(out.find("pre-push"), std::string::npos);
    EXPECT_EQ(out.find("notes push"), std::string::npos);
}

TEST(StatusCli, VerboseShowsSetupDiagnostics) {
    StatusRepo repo;

    auto result = ghost::test::run(repo.path, ghostBin(), {"--verbose", "status"});
    std::string out = ghost::test::output(result);
    std::string normalizedPath = repo.path;
    for (char& ch : normalizedPath) {
        if (ch == '\\') ch = '/';
    }

    EXPECT_EQ(result.exitCode, 0) << out;
    EXPECT_NE(out.find("Diagnostics"), std::string::npos);
    EXPECT_TRUE(out.find(repo.path) != std::string::npos || out.find(normalizedPath) != std::string::npos) << out;
    EXPECT_NE(out.find("post-commit"), std::string::npos);
    EXPECT_NE(out.find("pre-push"), std::string::npos);
    EXPECT_NE(out.find("notes push"), std::string::npos);
}
