#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <memory>
#include <string>
#include <chrono>

namespace fs = std::filesystem;

static std::string runCapture(const std::string& cmd, const std::string& cwd, int* exitCode = nullptr) {
    std::string fullCmd = "cd \"" + cwd + "\" && " + cmd + " 2>&1";
    FILE* pipe = popen(fullCmd.c_str(), "r");
    std::string result;
    if (pipe) {
        char buffer[512];
        while (fgets(buffer, sizeof(buffer), pipe)) result += buffer;
        int rc = pclose(pipe);
        if (exitCode) *exitCode = rc;
    } else if (exitCode) {
        *exitCode = -1;
    }
    return result;
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

class GovernanceRepo {
public:
    std::string path;

    GovernanceRepo() {
        auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(rand());
        path = (fs::temp_directory_path() / ("ghost-gov-test-" + suffix)).string();
        fs::create_directories(path);
        runCapture("git init", path);
        runCapture("git config user.name \"Owner\"", path);
        runCapture("git config user.email \"owner@example.com\"", path);
    }

    ~GovernanceRepo() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    std::string read(const std::string& rel) const {
        std::ifstream in(fs::path(path) / rel);
        std::stringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    void write(const std::string& rel, const std::string& content) {
        fs::path p = fs::path(path) / rel;
        fs::create_directories(p.parent_path());
        std::ofstream out(p);
        out << content;
    }
};

TEST(GovernanceCli, OwnerInitCreatesPolicyWorkflowGuideAndCodeowners) {
    GovernanceRepo repo;
    int rc = 0;
    runCapture("\"" + ghostBin() + "\" init --owner --mode restrictive --github-owner @owner --force", repo.path, &rc);

    EXPECT_EQ(rc, 0);
    EXPECT_TRUE(fs::exists(fs::path(repo.path) / "ghost.yml"));
    EXPECT_TRUE(fs::exists(fs::path(repo.path) / "GHOST.md"));
    EXPECT_TRUE(fs::exists(fs::path(repo.path) / ".github" / "workflows" / "ghost-audit.yml"));
    EXPECT_TRUE(fs::exists(fs::path(repo.path) / ".github" / "CODEOWNERS"));

    std::string cfg = repo.read("ghost.yml");
    EXPECT_NE(cfg.find("mode: restrictive"), std::string::npos);
    EXPECT_NE(cfg.find("owners:"), std::string::npos);
    EXPECT_NE(cfg.find("locked: false"), std::string::npos);

    std::string codeowners = repo.read(".github/CODEOWNERS");
    EXPECT_NE(codeowners.find("/ghost.yml @owner"), std::string::npos);
    EXPECT_NE(codeowners.find("/ghost-policy.sig @owner"), std::string::npos);
}

TEST(GovernanceCli, ContributorInitPreservesPolicy) {
    GovernanceRepo repo;
    int rc = 0;
    runCapture("\"" + ghostBin() + "\" init --owner --mode locked --github-owner @owner --force", repo.path, &rc);
    ASSERT_EQ(rc, 0);

    std::string before = repo.read("ghost.yml");
    runCapture("\"" + ghostBin() + "\" init --contributor", repo.path, &rc);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(repo.read("ghost.yml"), before);
}

TEST(GovernanceCli, PolicyLockBlocksProtectedChangesUntilUnlock) {
    GovernanceRepo repo;
    int rc = 0;
    runCapture("\"" + ghostBin() + "\" init --owner --mode restrictive --github-owner @owner --force", repo.path, &rc);
    ASSERT_EQ(rc, 0);

    runCapture("\"" + ghostBin() + "\" policy lock", repo.path, &rc);
    EXPECT_EQ(rc, 0);

    runCapture("\"" + ghostBin() + "\" policy set mode permissive", repo.path, &rc);
    EXPECT_NE(rc, 0);

    runCapture("\"" + ghostBin() + "\" policy unlock --force", repo.path, &rc);
    EXPECT_EQ(rc, 0);

    runCapture("\"" + ghostBin() + "\" policy set mode permissive", repo.path, &rc);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(repo.read("ghost.yml").find("mode: permissive"), std::string::npos);
}

TEST(GovernanceCli, PolicySignatureDetectsTampering) {
    GovernanceRepo repo;
    int rc = 0;
    runCapture("\"" + ghostBin() + "\" init --owner --mode restrictive --github-owner @owner --force", repo.path, &rc);
    ASSERT_EQ(rc, 0);

    runCapture("\"" + ghostBin() + "\" policy sign", repo.path, &rc);
    EXPECT_EQ(rc, 0);
    EXPECT_TRUE(fs::exists(fs::path(repo.path) / "ghost-policy.sig"));

    runCapture("\"" + ghostBin() + "\" policy verify", repo.path, &rc);
    EXPECT_EQ(rc, 0);

    std::ofstream out(fs::path(repo.path) / "ghost.yml", std::ios::app);
    out << "\n# tamper\n";
    out.close();

    runCapture("\"" + ghostBin() + "\" policy verify", repo.path, &rc);
    EXPECT_NE(rc, 0);
}

TEST(GovernanceCli, NoteSignatureDetectsTampering) {
    GovernanceRepo repo;
    int rc = 0;
    runCapture("\"" + ghostBin() + "\" init --owner --mode restrictive --github-owner @owner --force", repo.path, &rc);
    ASSERT_EQ(rc, 0);

    repo.write("file.txt", "hello\n");
    runCapture("git add -A && git commit -m init", repo.path, &rc);
    ASSERT_EQ(rc, 0);

    runCapture("\"" + ghostBin() + "\" post-commit", repo.path, &rc);
    EXPECT_EQ(rc, 0);

    runCapture("\"" + ghostBin() + "\" notes verify HEAD", repo.path, &rc);
    EXPECT_EQ(rc, 0);

    runCapture("git notes --ref=refs/notes/ghost-verified add -f -m tampered HEAD", repo.path, &rc);
    ASSERT_EQ(rc, 0);

    runCapture("\"" + ghostBin() + "\" notes verify HEAD", repo.path, &rc);
    EXPECT_NE(rc, 0);
}
