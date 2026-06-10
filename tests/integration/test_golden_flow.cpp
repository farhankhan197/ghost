#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <memory>
#include <string>
#include <chrono>
#include <cstdlib>
#include <vector>
#include "persist/db.hpp"

namespace fs = std::filesystem;

static std::string runCapture(const std::string& cmd, const std::string& cwd, int* exitCode = nullptr) {
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

static std::string quotePath(const fs::path& path) {
    return "\"" + path.string() + "\"";
}

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

static std::string checkpointBin() {
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

static std::string commitWithGhostHook(const std::string& message) {
    fs::path binDir = fs::path(ghostBin()).parent_path();
#ifdef _WIN32
    return "set \"GHOST_BIN=" + binDir.string() + "\" && git commit -m \"" + message + "\"";
#else
    return "GHOST_BIN=\"" + binDir.string() + "\" git commit -m \"" + message + "\"";
#endif
}

static std::string withGhostBin(const std::string& cmd) {
    fs::path binDir = fs::path(ghostBin()).parent_path();
#ifdef _WIN32
    return "set \"GHOST_BIN=" + binDir.string() + "\" && " + cmd;
#else
    return "GHOST_BIN=\"" + binDir.string() + "\" " + cmd;
#endif
}

class GoldenWorkspace {
public:
    fs::path root;
    fs::path remote;
    fs::path owner;
    fs::path contributor;
    fs::path auditor;

    GoldenWorkspace() {
        auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(rand());
        root = fs::temp_directory_path() / ("ghost-golden-flow-" + suffix);
        remote = root / "remote.git";
        owner = root / "owner";
        contributor = root / "contributor";
        auditor = root / "auditor";
        fs::create_directories(root);
    }

    ~GoldenWorkspace() {
        ghost::persist::closeRepoDb(owner.string());
        ghost::persist::closeRepoDb(contributor.string());
        ghost::persist::closeRepoDb(auditor.string());
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    void write(const fs::path& repo, const std::string& rel, const std::string& content) {
        fs::path p = repo / rel;
        fs::create_directories(p.parent_path());
        std::ofstream out(p);
        out << content;
    }
};

TEST(GoldenFlow, OwnerContributorCloneCaptureCommitPushFetchAndAudit) {
    GoldenWorkspace ws;
    int rc = 0;

    runCapture("git init --bare " + quotePath(ws.remote), ws.root.string(), &rc);
    ASSERT_EQ(rc, 0);

    fs::create_directories(ws.owner);
    runCapture("git init", ws.owner.string(), &rc);
    ASSERT_EQ(rc, 0);
    runCapture("git branch -M main", ws.owner.string(), &rc);
    ASSERT_EQ(rc, 0);
    runCapture("git config user.name \"Repo Owner\"", ws.owner.string(), &rc);
    ASSERT_EQ(rc, 0);
    runCapture("git config user.email \"owner@example.com\"", ws.owner.string(), &rc);
    ASSERT_EQ(rc, 0);
    runCapture("git remote add origin " + quotePath(ws.remote), ws.owner.string(), &rc);
    ASSERT_EQ(rc, 0);

    runCapture(quotePath(ghostBin()) + " init --owner --mode transparent --github-owner @owner --force", ws.owner.string(), &rc);
    ASSERT_EQ(rc, 0);
    ws.write(ws.owner, "src/app.txt", "base\n");
    runCapture("git add -A", ws.owner.string(), &rc);
    ASSERT_EQ(rc, 0);
    runCapture(commitWithGhostHook("owner policy"), ws.owner.string(), &rc);
    ASSERT_EQ(rc, 0);
    runCapture("git push --no-verify -u origin main", ws.owner.string(), &rc);
    ASSERT_EQ(rc, 0);
    runCapture("git --git-dir " + quotePath(ws.remote) + " symbolic-ref HEAD refs/heads/main", ws.root.string(), &rc);
    ASSERT_EQ(rc, 0);

    runCapture("git clone " + quotePath(ws.remote) + " " + quotePath(ws.contributor), ws.root.string(), &rc);
    ASSERT_EQ(rc, 0);
    runCapture("git config user.name \"AI Contributor\"", ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);
    runCapture("git config user.email \"contrib@example.com\"", ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);
    runCapture(quotePath(ghostBin()) + " init --contributor", ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);

    std::string policy = runCapture("git show HEAD:ghost.yml", ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);
    EXPECT_NE(policy.find("mode: transparent"), std::string::npos);

    runCapture(quotePath(checkpointBin()) + " pre --agent opencode --file src/feature.txt", ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);
    ws.write(ws.contributor, "src/feature.txt", "alpha\nbeta\ngamma\n");
    runCapture(quotePath(checkpointBin()) + " post --agent opencode --model golden-model --file src/feature.txt", ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);

    std::string status = runCapture(quotePath(ghostBin()) + " status", ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);
    EXPECT_NE(status.find("opencode/golden-model"), std::string::npos);
    EXPECT_NE(status.find("src/feature.txt"), std::string::npos);

    runCapture("git add src/feature.txt", ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);
    std::string check = runCapture(quotePath(ghostBin()) + " check --json", ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);
    EXPECT_NE(check.find("\"staged_files\": 1"), std::string::npos);
    EXPECT_NE(check.find("\"predicted_ai_additions\": 3"), std::string::npos);
    EXPECT_NE(check.find("golden-model"), std::string::npos);

    runCapture(commitWithGhostHook("add AI captured feature"), ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);

    std::string ghostNote = runCapture("git notes --ref=refs/notes/ghost show HEAD", ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);
    EXPECT_NE(ghostNote.find("src/feature.txt"), std::string::npos);
    EXPECT_NE(ghostNote.find("opencode"), std::string::npos);
    EXPECT_NE(ghostNote.find("golden-model"), std::string::npos);

    std::string verifiedNote = runCapture("git notes --ref=refs/notes/ghost-verified show HEAD", ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);
    EXPECT_NE(verifiedNote.find("ghost-verified/1.0.0"), std::string::npos);

    std::string signatureNote = runCapture("git notes --ref=refs/notes/ghost-signatures show HEAD", ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);
    EXPECT_NE(signatureNote.find("ghost-note-signature/1"), std::string::npos);

    std::string verifyPr = runCapture(quotePath(ghostBin()) + " verify-pr origin/main..HEAD --base origin/main --no-fetch --json", ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);
    EXPECT_NE(verifyPr.find("\"blocked\": false"), std::string::npos);
    EXPECT_NE(verifyPr.find("\"ai_lines\":"), std::string::npos);

    std::string pushBranch = runCapture(withGhostBin("git push origin main"), ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0) << pushBranch;
    std::string pushNotes = runCapture(
        "git push --no-verify origin "
        "refs/notes/ghost:refs/notes/ghost "
        "refs/notes/ghost-verified:refs/notes/ghost-verified "
        "refs/notes/ghost-signatures:refs/notes/ghost-signatures",
        ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0) << pushNotes;

    runCapture("git clone " + quotePath(ws.remote) + " " + quotePath(ws.auditor), ws.root.string(), &rc);
    ASSERT_EQ(rc, 0);
    runCapture("git fetch origin refs/notes/ghost:refs/notes/ghost", ws.auditor.string(), &rc);
    ASSERT_EQ(rc, 0);
    runCapture("git fetch origin refs/notes/ghost-verified:refs/notes/ghost-verified", ws.auditor.string(), &rc);
    ASSERT_EQ(rc, 0);
    runCapture("git fetch origin refs/notes/ghost-signatures:refs/notes/ghost-signatures", ws.auditor.string(), &rc);
    ASSERT_EQ(rc, 0);

    std::string audit = runCapture(quotePath(ghostBin()) + " audit --range HEAD~1..HEAD --config-ref origin/main --json", ws.auditor.string(), &rc);
    ASSERT_EQ(rc, 0);
    EXPECT_NE(audit.find("\"blocked\": false"), std::string::npos);
    EXPECT_NE(audit.find("\"ai_lines\": 3"), std::string::npos);
    EXPECT_NE(audit.find("\"has_ghost_note\": true"), std::string::npos);
    EXPECT_NE(audit.find("\"has_verified_note\": true"), std::string::npos);
}
