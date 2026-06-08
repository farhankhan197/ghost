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

static std::string envSigningKeyPrefix(const fs::path& keyPath) {
#ifdef _WIN32
    return "set \"GHOST_SIGNING_KEY=" + keyPath.string() + "\" && ";
#else
    return "GHOST_SIGNING_KEY=\"" + keyPath.string() + "\" ";
#endif
}

static bool hasSshKeygen() {
    int rc = 0;
#ifdef _WIN32
    runCapture("where ssh-keygen", ".", &rc);
#else
    runCapture("which ssh-keygen", ".", &rc);
#endif
    return rc == 0;
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

    void append(const std::string& rel, const std::string& content) {
        std::ofstream out(fs::path(path) / rel, std::ios::app);
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

TEST(GovernanceCli, InitAutoDetectsOwnerWhenRepoHasNoRemotePolicy) {
    GovernanceRepo repo;
    int rc = 0;
    std::string out = runCapture("\"" + ghostBin() + "\" init", repo.path, &rc);

    EXPECT_EQ(rc, 0) << out;
    EXPECT_NE(out.find("Detected repo role: owner"), std::string::npos);
    EXPECT_TRUE(fs::exists(fs::path(repo.path) / "ghost.yml"));
    EXPECT_TRUE(fs::exists(fs::path(repo.path) / ".github" / "CODEOWNERS"));

    std::string cfg = repo.read("ghost.yml");
    EXPECT_NE(cfg.find("mode: restrictive"), std::string::npos);
    EXPECT_NE(cfg.find("owner: owner@example.com"), std::string::npos);
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

TEST(GovernanceCli, InitAutoDetectsContributorAndPreservesOwnerPolicy) {
    GovernanceRepo repo;
    int rc = 0;
    runCapture("\"" + ghostBin() + "\" init --owner --mode locked --github-owner @owner --force", repo.path, &rc);
    ASSERT_EQ(rc, 0);
    std::string before = repo.read("ghost.yml");

    runCapture("git config user.name \"Contributor\"", repo.path, &rc);
    ASSERT_EQ(rc, 0);
    runCapture("git config user.email \"contrib@example.com\"", repo.path, &rc);
    ASSERT_EQ(rc, 0);

    std::string out = runCapture("\"" + ghostBin() + "\" init", repo.path, &rc);
    EXPECT_EQ(rc, 0) << out;
    EXPECT_NE(out.find("Detected repo role: contributor"), std::string::npos);
    EXPECT_EQ(repo.read("ghost.yml"), before);
}

TEST(GovernanceCli, ExplicitOwnerInitIsBlockedForNonOwnerPolicy) {
    GovernanceRepo repo;
    int rc = 0;
    runCapture("\"" + ghostBin() + "\" init --owner --mode restrictive --github-owner @owner --force", repo.path, &rc);
    ASSERT_EQ(rc, 0);

    runCapture("git config user.name \"Contributor\"", repo.path, &rc);
    ASSERT_EQ(rc, 0);
    runCapture("git config user.email \"contrib@example.com\"", repo.path, &rc);
    ASSERT_EQ(rc, 0);

    std::string out = runCapture("\"" + ghostBin() + "\" init --owner --mode permissive", repo.path, &rc);
    EXPECT_NE(rc, 0);
    EXPECT_NE(out.find("owned by someone else"), std::string::npos);
    EXPECT_NE(repo.read("ghost.yml").find("mode: restrictive"), std::string::npos);
}

TEST(GovernanceCli, RemoteAuthorityOverridesTamperedLocalPolicyOwner) {
    GovernanceRepo repo;
    int rc = 0;
    runCapture("\"" + ghostBin() + "\" init --owner --mode restrictive --github-owner @actual-owner --force", repo.path, &rc);
    ASSERT_EQ(rc, 0);
    runCapture("git remote set-url origin https://github.com/actual-owner/example.git", repo.path, &rc);
    ASSERT_EQ(rc, 0);

    runCapture("git config user.name \"Contributor\"", repo.path, &rc);
    ASSERT_EQ(rc, 0);
    runCapture("git config user.email \"contrib@example.com\"", repo.path, &rc);
    ASSERT_EQ(rc, 0);

    std::string tampered =
        "# Ghost configuration\n"
        "version: 1\n"
        "mode: permissive\n"
        "locked: false\n"
        "threshold: 100\n"
        "required: false\n"
        "on_exceed: allow\n"
        "pr_comment: true\n"
        "untagged: human\n"
        "unverified: warn\n"
        "gitai_fb: true\n"
        "owner: contrib@example.com\n"
        "owners:\n"
        "  - contrib@example.com\n";
    repo.write("ghost.yml", tampered);

    std::string out = runCapture("\"" + ghostBin() + "\" init", repo.path, &rc);
    EXPECT_EQ(rc, 0) << out;
    EXPECT_NE(out.find("Detected repo role: contributor"), std::string::npos);
    EXPECT_EQ(repo.read("ghost.yml"), tampered);
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

TEST(GovernanceCli, TrustedPolicySignatureUsesSshKey) {
    if (!hasSshKeygen()) GTEST_SKIP() << "ssh-keygen not available";
    GovernanceRepo repo;
    int rc = 0;
    runCapture("\"" + ghostBin() + "\" init --owner --mode restrictive --github-owner @owner --force", repo.path, &rc);
    ASSERT_EQ(rc, 0);

    fs::path keyPath = fs::path(repo.path) / "keys" / "id_ed25519";
    fs::create_directories(keyPath.parent_path());
    runCapture("ssh-keygen -t ed25519 -N \"\" -C owner@example.com -f \"" + keyPath.string() + "\"", repo.path, &rc);
    ASSERT_EQ(rc, 0);
    std::string pub = repo.read("keys/id_ed25519.pub");
    ASSERT_FALSE(pub.empty());
    while (!pub.empty() && (pub.back() == '\n' || pub.back() == '\r')) pub.pop_back();

    repo.append("ghost.yml",
        "trusted_signers:\n"
        "  - name: Owner\n"
        "    email: owner@example.com\n"
        "    github: owner\n"
        "    ssh_key: " + pub + "\n");

    std::string signOut = runCapture(envSigningKeyPrefix(keyPath) + "\"" + ghostBin() + "\" policy sign", repo.path, &rc);
    ASSERT_EQ(rc, 0) << signOut;
    std::string sig = repo.read("ghost-policy.sig");
    EXPECT_NE(sig.find("schema: ghost-policy-signature/2"), std::string::npos);
    EXPECT_NE(sig.find("signature_b64:"), std::string::npos);

    std::string verifyOut = runCapture("\"" + ghostBin() + "\" policy verify --trusted", repo.path, &rc);
    EXPECT_EQ(rc, 0) << verifyOut;

    repo.append("ghost.yml", "\n# tamper\n");
    verifyOut = runCapture("\"" + ghostBin() + "\" policy verify --trusted", repo.path, &rc);
    EXPECT_NE(rc, 0);
}

TEST(GovernanceCli, TrustedPolicySignRejectsUntrustedSshKey) {
    if (!hasSshKeygen()) GTEST_SKIP() << "ssh-keygen not available";
    GovernanceRepo repo;
    int rc = 0;
    runCapture("\"" + ghostBin() + "\" init --owner --mode restrictive --github-owner @owner --force", repo.path, &rc);
    ASSERT_EQ(rc, 0);

    fs::path trustedKey = fs::path(repo.path) / "keys" / "trusted";
    fs::path untrustedKey = fs::path(repo.path) / "keys" / "untrusted";
    fs::create_directories(trustedKey.parent_path());
    runCapture("ssh-keygen -t ed25519 -N \"\" -C owner@example.com -f \"" + trustedKey.string() + "\"", repo.path, &rc);
    ASSERT_EQ(rc, 0);
    runCapture("ssh-keygen -t ed25519 -N \"\" -C other@example.com -f \"" + untrustedKey.string() + "\"", repo.path, &rc);
    ASSERT_EQ(rc, 0);
    std::string pub = repo.read("keys/trusted.pub");
    while (!pub.empty() && (pub.back() == '\n' || pub.back() == '\r')) pub.pop_back();

    repo.append("ghost.yml",
        "trusted_signers:\n"
        "  - name: Owner\n"
        "    email: owner@example.com\n"
        "    ssh_key: " + pub + "\n");

    std::string out = runCapture(envSigningKeyPrefix(untrustedKey) + "\"" + ghostBin() + "\" policy sign", repo.path, &rc);
    EXPECT_NE(rc, 0);
    EXPECT_NE(out.find("not listed in trusted_signers"), std::string::npos);
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

TEST(GovernanceCli, TrustedNoteSignatureUsesSshKey) {
    if (!hasSshKeygen()) GTEST_SKIP() << "ssh-keygen not available";
    GovernanceRepo repo;
    int rc = 0;
    runCapture("\"" + ghostBin() + "\" init --owner --mode restrictive --github-owner @owner --force", repo.path, &rc);
    ASSERT_EQ(rc, 0);

    fs::path keyPath = fs::path(repo.path) / "keys" / "id_ed25519";
    fs::create_directories(keyPath.parent_path());
    runCapture("ssh-keygen -t ed25519 -N \"\" -C owner@example.com -f \"" + keyPath.string() + "\"", repo.path, &rc);
    ASSERT_EQ(rc, 0);
    std::string pub = repo.read("keys/id_ed25519.pub");
    while (!pub.empty() && (pub.back() == '\n' || pub.back() == '\r')) pub.pop_back();

    repo.append("ghost.yml",
        "trusted_signers:\n"
        "  - name: Owner\n"
        "    email: owner@example.com\n"
        "    ssh_key: " + pub + "\n");

    repo.write("file.txt", "hello\n");
    runCapture("git add -A && git commit -m init", repo.path, &rc);
    ASSERT_EQ(rc, 0);

    std::string postOut = runCapture(envSigningKeyPrefix(keyPath) + "\"" + ghostBin() + "\" post-commit", repo.path, &rc);
    ASSERT_EQ(rc, 0) << postOut;

    std::string noteSig = runCapture("git notes --ref=refs/notes/ghost-signatures show HEAD", repo.path, &rc);
    ASSERT_EQ(rc, 0);
    EXPECT_NE(noteSig.find("schema: ghost-note-signature/2"), std::string::npos);

    std::string verifyOut = runCapture("\"" + ghostBin() + "\" notes verify HEAD --trusted", repo.path, &rc);
    EXPECT_EQ(rc, 0) << verifyOut;
}
