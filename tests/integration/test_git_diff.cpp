#include <gtest/gtest.h>
#include "diff.hpp"
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <memory>
#include <chrono>

namespace fs = std::filesystem;

static std::string runCapture(const std::string& cmd, const std::string& cwd) {
    std::string fullCmd = "cd \"" + cwd + "\" && " + cmd + " 2>&1";
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(fullCmd.c_str(), "r"), pclose);
    std::string result;
    if (!pipe) return result;
    char buffer[512];
    while (fgets(buffer, sizeof(buffer), pipe.get())) result += buffer;
    return result;
}

class DiffRepo {
public:
    std::string path;

    DiffRepo() {
        auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(rand());
        path = (fs::temp_directory_path() / ("ghost-diff-test-" + suffix)).string();
        fs::create_directories(path);
        runCapture("git init", path);
        runCapture("git config user.name \"Diff User\"", path);
        runCapture("git config user.email \"diff@example.com\"", path);
    }

    ~DiffRepo() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    void write(const std::string& rel, const std::string& content) {
        fs::path p = fs::path(path) / rel;
        fs::create_directories(p.parent_path());
        std::ofstream out(p);
        out << content;
    }

    void commit(const std::string& message) {
        runCapture("git add -A", path);
        runCapture("git commit -m \"" + message + "\"", path);
    }
};

TEST(GitDiff, CommitRangesParseAddedLines) {
    DiffRepo repo;
    repo.write("src/app.txt", "one\ntwo\nthree\n");
    repo.commit("initial");

    repo.write("src/app.txt", "zero\ntwo\nthree\nfour\n");
    repo.commit("edit");

    auto ranges = ghost::git::Diff::getCommitRanges(repo.path, "HEAD");

    ASSERT_TRUE(ranges.added.count("src/app.txt"));
    EXPECT_EQ(ranges.added["src/app.txt"].toString(), "1,4");
    ASSERT_TRUE(ranges.deleted.count("src/app.txt"));
    EXPECT_EQ(ranges.deleted["src/app.txt"].toString(), "1");
}

TEST(GitDiff, CommitRangesTrackRenames) {
    DiffRepo repo;
    repo.write("old.txt", "one\ntwo\n");
    repo.commit("initial");

    runCapture("git mv old.txt new.txt", repo.path);
    repo.write("new.txt", "one\ntwo\nthree\n");
    repo.commit("rename edit");

    auto ranges = ghost::git::Diff::getCommitRanges(repo.path, "HEAD");

    ASSERT_TRUE(ranges.renames.count("new.txt"));
    EXPECT_EQ(ranges.renames["new.txt"], "old.txt");
    ASSERT_TRUE(ranges.added.count("new.txt"));
    EXPECT_EQ(ranges.added["new.txt"].toString(), "3");
}

TEST(GitDiff, StagedRangesParseCachedChanges) {
    DiffRepo repo;
    repo.write("app.txt", "one\ntwo\n");
    repo.commit("initial");

    repo.write("app.txt", "one\ntwo\nthree\nfour\n");
    runCapture("git add app.txt", repo.path);

    auto ranges = ghost::git::Diff::getChangedRanges(repo.path, "--cached");

    ASSERT_TRUE(ranges.added.count("app.txt"));
    EXPECT_EQ(ranges.added["app.txt"].toString(), "3-4");
}
