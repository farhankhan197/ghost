#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <memory>
#include <string>
#include <chrono>
#include <vector>
#include "persist/db.hpp"

namespace fs = std::filesystem;

static std::string runCaptureFinalDiff(const std::string& cmd, const std::string& cwd, int* exitCode = nullptr) {
    std::string fullCmd = "cd \"" + cwd + "\" && " + cmd + " 2>&1";
    FILE* pipe = popen(fullCmd.c_str(), "r");
    std::string result;
    if (pipe) {
        char buffer[1024];
        while (fgets(buffer, sizeof(buffer), pipe)) result += buffer;
        int rc = pclose(pipe);
        if (exitCode) *exitCode = rc;
    } else if (exitCode) {
        *exitCode = -1;
    }
    return result;
}

static std::string quoteFinalDiff(const fs::path& path) {
    return "\"" + path.string() + "\"";
}

static std::string ghostBinFinalDiff() {
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

static std::string checkpointBinFinalDiff() {
#ifdef _WIN32
    std::vector<fs::path> candidates = {
        fs::current_path() / "ghost-checkpoint.exe",
        fs::current_path() / "build" / "ghost-checkpoint.exe",
        fs::current_path().parent_path() / "ghost-checkpoint.exe"
    };
#else
    std::vector<fs::path> candidates = {
        fs::current_path() / "ghost-checkpoint",
        fs::current_path() / "build" / "ghost-checkpoint",
        fs::current_path().parent_path() / "ghost-checkpoint"
    };
#endif
    for (const auto& candidate : candidates) {
        if (fs::exists(candidate)) return candidate.string();
    }
    return candidates.front().string();
}

class FinalDiffRepo {
public:
    fs::path path;

    FinalDiffRepo() {
        auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        path = fs::temp_directory_path() / ("ghost-final-diff-" + suffix);
        fs::create_directories(path);
        runCaptureFinalDiff("git init", path.string());
        runCaptureFinalDiff("git branch -M main", path.string());
        runCaptureFinalDiff("git config user.name \"Final Diff Tester\"", path.string());
        runCaptureFinalDiff("git config user.email \"final@example.com\"", path.string());
    }

    ~FinalDiffRepo() {
        ghost::persist::closeRepoDb(path.string());
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    void write(const std::string& rel, const std::string& content) {
        fs::path p = path / rel;
        fs::create_directories(p.parent_path());
        std::ofstream out(p);
        out << content;
    }
};

static void writeFinalDiffPolicy(FinalDiffRepo& repo) {
    repo.write("ghost.yml",
        "version: 1\n"
        "mode: restrictive\n"
        "locked: false\n"
        "threshold: 20\n"
        "required: true\n"
        "on_exceed: block\n"
        "pr_comment: true\n"
        "untagged: human\n"
        "unverified: warn\n"
        "gitai_fb: true\n"
        "enforcement:\n"
        "  scope: final_diff\n"
        "  history: warn\n"
        "ignore:\n"
        "  - .git/\n");
}

static void commitAll(FinalDiffRepo& repo, const std::string& message) {
    int rc = 0;
    runCaptureFinalDiff("git add -A", repo.path.string(), &rc);
    ASSERT_EQ(rc, 0);
    runCaptureFinalDiff("git commit -m \"" + message + "\"", repo.path.string(), &rc);
    ASSERT_EQ(rc, 0);
    runCaptureFinalDiff(quoteFinalDiff(ghostBinFinalDiff()) + " post-commit", repo.path.string(), &rc);
    ASSERT_EQ(rc, 0);
}

static std::string makeLines(const std::string& prefix, int count) {
    std::string out;
    for (int i = 1; i <= count; ++i) {
        out += prefix + " " + std::to_string(i) + "\n";
    }
    return out;
}

TEST(FinalDiff, RemovedAiScaffoldDoesNotBlockCleanFinalDiff) {
    FinalDiffRepo repo;
    int rc = 0;
    writeFinalDiffPolicy(repo);
    repo.write("src/base.txt", "base\n");
    commitAll(repo, "base policy");
    std::string base = runCaptureFinalDiff("git rev-parse HEAD", repo.path.string(), &rc);
    ASSERT_EQ(rc, 0);
    while (!base.empty() && (base.back() == '\n' || base.back() == '\r')) base.pop_back();

    runCaptureFinalDiff(quoteFinalDiff(checkpointBinFinalDiff()) + " pre --agent codex --file src/generated.txt", repo.path.string(), &rc);
    ASSERT_EQ(rc, 0);
    repo.write("src/generated.txt", makeLines("ai scaffold", 50));
    runCaptureFinalDiff(quoteFinalDiff(checkpointBinFinalDiff()) + " post --agent codex --model test-model --file src/generated.txt", repo.path.string(), &rc);
    ASSERT_EQ(rc, 0);
    commitAll(repo, "add ai scaffold");

    fs::remove(repo.path / "src/generated.txt");
    commitAll(repo, "remove scaffold");

    std::string out = runCaptureFinalDiff(
        quoteFinalDiff(ghostBinFinalDiff()) + " verify-pr " + base + "..HEAD --base " + base + " --no-fetch --json",
        repo.path.string(),
        &rc);
    EXPECT_EQ(rc, 0) << out;
    EXPECT_NE(out.find("\"blocked\": false"), std::string::npos) << out;
    EXPECT_NE(out.find("\"total_lines\": 0"), std::string::npos) << out;
}

TEST(FinalDiff, SurvivingAiLinesStillBlock) {
    FinalDiffRepo repo;
    int rc = 0;
    writeFinalDiffPolicy(repo);
    repo.write("src/base.txt", "base\n");
    commitAll(repo, "base policy");
    std::string base = runCaptureFinalDiff("git rev-parse HEAD", repo.path.string(), &rc);
    ASSERT_EQ(rc, 0);
    while (!base.empty() && (base.back() == '\n' || base.back() == '\r')) base.pop_back();

    runCaptureFinalDiff(quoteFinalDiff(checkpointBinFinalDiff()) + " pre --agent codex --file src/generated.txt", repo.path.string(), &rc);
    ASSERT_EQ(rc, 0);
    repo.write("src/generated.txt", makeLines("ai scaffold", 50));
    runCaptureFinalDiff(quoteFinalDiff(checkpointBinFinalDiff()) + " post --agent codex --model test-model --file src/generated.txt", repo.path.string(), &rc);
    ASSERT_EQ(rc, 0);
    commitAll(repo, "add ai scaffold");

    std::string out = runCaptureFinalDiff(
        quoteFinalDiff(ghostBinFinalDiff()) + " verify-pr " + base + "..HEAD --base " + base + " --no-fetch --json",
        repo.path.string(),
        &rc);
    EXPECT_NE(rc, 0) << out;
    EXPECT_NE(out.find("\"blocked\": true"), std::string::npos) << out;
    EXPECT_NE(out.find("\"ai_lines\": 50"), std::string::npos) << out;
}
