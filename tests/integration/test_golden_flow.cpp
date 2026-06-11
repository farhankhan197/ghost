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

static void initSimpleRepo(const fs::path& repo) {
    int rc = 0;
    fs::create_directories(repo);
    runCapture("git init", repo.string(), &rc);
    ASSERT_EQ(rc, 0);
    runCapture("git config user.name \"Flow Tester\"", repo.string(), &rc);
    ASSERT_EQ(rc, 0);
    runCapture("git config user.email \"flow@example.com\"", repo.string(), &rc);
    ASSERT_EQ(rc, 0);
}

static std::string trimNewlines(std::string value) {
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) {
        value.pop_back();
    }
    return value;
}

static void commitAiEdit(
    GoldenWorkspace& ws,
    const fs::path& repo,
    const std::string& agent,
    const std::string& model,
    const std::string& file,
    const std::string& content,
    const std::string& message
) {
    int rc = 0;
    runCapture(quotePath(checkpointBin()) + " pre --agent " + agent + " --file " + file, repo.string(), &rc);
    ASSERT_EQ(rc, 0);
    ws.write(repo, file, content);
    runCapture(quotePath(checkpointBin()) + " post --agent " + agent + " --model " + model + " --file " + file, repo.string(), &rc);
    ASSERT_EQ(rc, 0);
    runCapture("git add " + file, repo.string(), &rc);
    ASSERT_EQ(rc, 0);
    runCapture("git commit -m \"" + message + "\"", repo.string(), &rc);
    ASSERT_EQ(rc, 0);
    runCapture(quotePath(ghostBin()) + " post-commit", repo.string(), &rc);
    ASSERT_EQ(rc, 0);
}

static void commitHumanEdit(
    GoldenWorkspace& ws,
    const fs::path& repo,
    const std::string& file,
    const std::string& content,
    const std::string& message
) {
    int rc = 0;
    ws.write(repo, file, content);
    runCapture("git add " + file, repo.string(), &rc);
    ASSERT_EQ(rc, 0);
    runCapture("git commit -m \"" + message + "\"", repo.string(), &rc);
    ASSERT_EQ(rc, 0);
    runCapture(quotePath(ghostBin()) + " post-commit", repo.string(), &rc);
    ASSERT_EQ(rc, 0);
}

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

TEST(GoldenFlow, FinalCodebaseAttributionSurvivesMultiAgentOverwrite) {
    GoldenWorkspace ws;
    int rc = 0;
    fs::create_directories(ws.contributor);

    runCapture("git init", ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);
    runCapture("git config user.name \"Flow Tester\"", ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);
    runCapture("git config user.email \"flow@example.com\"", ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);

    runCapture(quotePath(checkpointBin()) + " pre --agent codex --file a.txt", ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);
    ws.write(ws.contributor, "a.txt",
        "codex 1\ncodex 2\ncodex 3\ncodex 4\ncodex 5\n"
        "codex 6\ncodex 7\ncodex 8\ncodex 9\ncodex 10\n");
    runCapture(quotePath(checkpointBin()) + " post --agent codex --model codex-model --file a.txt", ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);
    runCapture("git add a.txt", ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);
    runCapture("git commit -m \"codex writes ten\"", ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);
    runCapture(quotePath(ghostBin()) + " post-commit", ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);
    std::string codexSha = runCapture("git rev-parse HEAD", ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);
    codexSha.erase(codexSha.find_last_not_of("\r\n") + 1);

    runCapture(quotePath(checkpointBin()) + " pre --agent opencode --file a.txt", ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);
    ws.write(ws.contributor, "a.txt",
        "open 1\nopen 2\nopen 3\nopen 4\nopen 5\n"
        "codex 6\ncodex 7\ncodex 8\ncodex 9\ncodex 10\n");
    runCapture(quotePath(checkpointBin()) + " post --agent opencode --model open-model --file a.txt", ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);
    runCapture("git add a.txt", ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);
    runCapture("git commit -m \"opencode overwrites five\"", ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);
    runCapture(quotePath(ghostBin()) + " post-commit", ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);
    std::string opencodeSha = runCapture("git rev-parse HEAD", ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);
    opencodeSha.erase(opencodeSha.find_last_not_of("\r\n") + 1);

    ws.write(ws.contributor, "a.txt",
        "open 1\nopen 2\nopen 3\nopen 4\nopen 5\n"
        "codex 6\ncodex 7\ncodex 8\ncodex 9\ncodex 10\n"
        "human 11\nhuman 12\nhuman 13\nhuman 14\nhuman 15\n"
        "human 16\nhuman 17\nhuman 18\nhuman 19\nhuman 20\n");
    runCapture("git add a.txt", ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);
    runCapture("git commit -m \"human appends ten\"", ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);
    runCapture(quotePath(ghostBin()) + " post-commit", ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);
    std::string humanSha = runCapture("git rev-parse HEAD", ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);
    humanSha.erase(humanSha.find_last_not_of("\r\n") + 1);

    std::string codexNote = runCapture("git notes --ref=refs/notes/ghost show " + codexSha, ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);
    EXPECT_NE(codexNote.find("a.txt"), std::string::npos);
    EXPECT_NE(codexNote.find(" 1-10"), std::string::npos);
    EXPECT_NE(codexNote.find("\"agent\": \"codex\""), std::string::npos);
    EXPECT_NE(codexNote.find("\"model\": \"codex-model\""), std::string::npos);
    EXPECT_NE(codexNote.find("\"author\": \"Flow Tester <flow@example.com>\""), std::string::npos);

    std::string opencodeNote = runCapture("git notes --ref=refs/notes/ghost show " + opencodeSha, ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);
    EXPECT_NE(opencodeNote.find("a.txt"), std::string::npos);
    EXPECT_NE(opencodeNote.find(" 1-5"), std::string::npos);
    EXPECT_NE(opencodeNote.find("\"agent\": \"opencode\""), std::string::npos);
    EXPECT_NE(opencodeNote.find("\"model\": \"open-model\""), std::string::npos);
    EXPECT_NE(opencodeNote.find("\"author\": \"Flow Tester <flow@example.com>\""), std::string::npos);

    std::string humanGhostNote = runCapture("git notes --ref=refs/notes/ghost show " + humanSha, ws.contributor.string(), &rc);
    EXPECT_NE(rc, 0);
    EXPECT_TRUE(humanGhostNote.find("no note found") != std::string::npos ||
                humanGhostNote.find("No note found") != std::string::npos ||
                humanGhostNote.find("error") != std::string::npos);

    std::string humanVerifiedNote = runCapture("git notes --ref=refs/notes/ghost-verified show " + humanSha, ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);
    EXPECT_NE(humanVerifiedNote.find("ghost-verified/1.0.0"), std::string::npos);

    std::string blame = runCapture(quotePath(ghostBin()) + " blame a.txt --json", ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);
    EXPECT_NE(blame.find("\"total_lines\": 20"), std::string::npos);
    EXPECT_NE(blame.find("\"ai_lines\": 10"), std::string::npos);
    EXPECT_NE(blame.find("\"agent\": \"codex\""), std::string::npos);
    EXPECT_NE(blame.find("\"model\": \"codex-model\""), std::string::npos);
    EXPECT_NE(blame.find("\"agent\": \"opencode\""), std::string::npos);
    EXPECT_NE(blame.find("\"model\": \"open-model\""), std::string::npos);

    std::string audit = runCapture(quotePath(ghostBin()) + " audit --threshold 100 --json", ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);
    EXPECT_NE(audit.find("\"total_lines\": 20"), std::string::npos);
    EXPECT_NE(audit.find("\"ai_lines\": 10"), std::string::npos);
    EXPECT_NE(audit.find("\"path\": \"a.txt\""), std::string::npos);
    EXPECT_NE(audit.find("{\"agent\": \"codex\", \"model\": \"codex-model\", \"lines\": 5}"), std::string::npos);
    EXPECT_NE(audit.find("{\"agent\": \"opencode\", \"model\": \"open-model\", \"lines\": 5}"), std::string::npos);
}

TEST(GoldenFlow, HumanEditRemovesAiOwnershipForEditedLines) {
    GoldenWorkspace ws;
    initSimpleRepo(ws.contributor);
    int rc = 0;

    commitAiEdit(ws, ws.contributor, "codex", "codex-model", "a.txt",
        "ai 1\nai 2\nai 3\nai 4\nai 5\n"
        "ai 6\nai 7\nai 8\nai 9\nai 10\n",
        "codex writes ten");

    commitHumanEdit(ws, ws.contributor, "a.txt",
        "human 1\nhuman 2\nhuman 3\nai 4\nai 5\n"
        "ai 6\nai 7\nai 8\nai 9\nai 10\n",
        "human rewrites three");

    std::string blame = runCapture(quotePath(ghostBin()) + " blame a.txt --json", ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);
    EXPECT_NE(blame.find("\"total_lines\": 10"), std::string::npos);
    EXPECT_NE(blame.find("\"ai_lines\": 7"), std::string::npos);

    std::string audit = runCapture(quotePath(ghostBin()) + " audit --threshold 100 --json", ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);
    EXPECT_NE(audit.find("\"total_lines\": 10"), std::string::npos);
    EXPECT_NE(audit.find("\"ai_lines\": 7"), std::string::npos);
    EXPECT_NE(audit.find("{\"agent\": \"codex\", \"model\": \"codex-model\", \"lines\": 7}"), std::string::npos);
}

TEST(GoldenFlow, DeletedAiLinesDoNotCountInFinalCodebase) {
    GoldenWorkspace ws;
    initSimpleRepo(ws.contributor);
    int rc = 0;

    commitAiEdit(ws, ws.contributor, "codex", "codex-model", "a.txt",
        "ai 1\nai 2\nai 3\nai 4\nai 5\n"
        "ai 6\nai 7\nai 8\nai 9\nai 10\n",
        "codex writes ten");

    commitHumanEdit(ws, ws.contributor, "a.txt",
        "ai 1\nai 2\nai 3\nai 4\nai 5\nai 6\n",
        "human deletes four");

    std::string audit = runCapture(quotePath(ghostBin()) + " audit --threshold 100 --json", ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);
    EXPECT_NE(audit.find("\"total_lines\": 6"), std::string::npos);
    EXPECT_NE(audit.find("\"ai_lines\": 6"), std::string::npos);
    EXPECT_NE(audit.find("{\"agent\": \"codex\", \"model\": \"codex-model\", \"lines\": 6}"), std::string::npos);
}

TEST(GoldenFlow, FreshCloneReconstructsAttributionFromFetchedNotes) {
    GoldenWorkspace ws;
    int rc = 0;

    runCapture("git init --bare " + quotePath(ws.remote), ws.root.string(), &rc);
    ASSERT_EQ(rc, 0);
    initSimpleRepo(ws.contributor);
    runCapture("git branch -M main", ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);
    runCapture("git remote add origin " + quotePath(ws.remote), ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);

    commitAiEdit(ws, ws.contributor, "opencode", "open-model", "a.txt",
        "open 1\nopen 2\nopen 3\nopen 4\n",
        "opencode writes four");

    runCapture("git push --no-verify -u origin main", ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);
    runCapture(
        "git push --no-verify origin "
        "refs/notes/ghost:refs/notes/ghost "
        "refs/notes/ghost-verified:refs/notes/ghost-verified "
        "refs/notes/ghost-signatures:refs/notes/ghost-signatures",
        ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);
    runCapture("git --git-dir " + quotePath(ws.remote) + " symbolic-ref HEAD refs/heads/main", ws.root.string(), &rc);
    ASSERT_EQ(rc, 0);

    runCapture("git clone " + quotePath(ws.remote) + " " + quotePath(ws.auditor), ws.root.string(), &rc);
    ASSERT_EQ(rc, 0);
    runCapture("git fetch origin refs/notes/ghost:refs/notes/ghost", ws.auditor.string(), &rc);
    ASSERT_EQ(rc, 0);
    runCapture("git fetch origin refs/notes/ghost-verified:refs/notes/ghost-verified", ws.auditor.string(), &rc);
    ASSERT_EQ(rc, 0);
    runCapture("git fetch origin refs/notes/ghost-signatures:refs/notes/ghost-signatures", ws.auditor.string(), &rc);
    ASSERT_EQ(rc, 0);

    std::string audit = runCapture(quotePath(ghostBin()) + " audit --threshold 100 --json", ws.auditor.string(), &rc);
    ASSERT_EQ(rc, 0);
    EXPECT_NE(audit.find("\"total_lines\": 4"), std::string::npos);
    EXPECT_NE(audit.find("\"ai_lines\": 4"), std::string::npos);
    EXPECT_NE(audit.find("{\"agent\": \"opencode\", \"model\": \"open-model\", \"lines\": 4}"), std::string::npos);
}

TEST(GoldenFlow, DuplicateCapturedSessionsDoNotDoubleCount) {
    GoldenWorkspace ws;
    initSimpleRepo(ws.contributor);
    int rc = 0;

    std::string pre = quotePath(checkpointBin()) + " pre --agent codex --file a.txt";
    std::string post = quotePath(checkpointBin()) + " post --agent codex --model codex-model --file a.txt";
    runCapture(pre, ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);
    ws.write(ws.contributor, "a.txt", "ai 1\nai 2\nai 3\n");
    runCapture(post, ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);
    runCapture(pre, ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);
    ws.write(ws.contributor, "a.txt", "ai 1\nai 2\nai 3\n");
    runCapture(post, ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);

    runCapture("git add a.txt", ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);
    runCapture("git commit -m \"codex writes duplicate captured file\"", ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);
    runCapture(quotePath(ghostBin()) + " post-commit", ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);

    std::string audit = runCapture(quotePath(ghostBin()) + " audit --threshold 100 --json", ws.contributor.string(), &rc);
    ASSERT_EQ(rc, 0);
    EXPECT_NE(audit.find("\"total_lines\": 3"), std::string::npos);
    EXPECT_NE(audit.find("\"ai_lines\": 3"), std::string::npos);
    EXPECT_NE(audit.find("{\"agent\": \"codex\", \"model\": \"codex-model\", \"lines\": 3}"), std::string::npos);
}
