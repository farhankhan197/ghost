#include <gtest/gtest.h>
#include "ghost_config.hpp"
#include <filesystem>
#include <fstream>
#include <cstdio>

namespace fs = std::filesystem;
using ghost::config::GhostConfigReader;
using ghost::config::GhostConfig;

class TempDir {
public:
    std::string path;
    
    TempDir() {
        auto tmp = fs::temp_directory_path() / "ghost-config-test-XXXXXX";
        path = tmp.string();
        fs::create_directories(path);
    }
    
    ~TempDir() {
        if (fs::exists(path)) {
            fs::remove_all(path);
        }
    }
};

TEST(GhostConfig, LoadDefaultsWhenMissing) {
    TempDir tmp;
    auto cfg = GhostConfigReader::load(tmp.path);
    
    EXPECT_EQ(cfg.version, 1);
    EXPECT_FALSE(cfg.required);
    EXPECT_EQ(cfg.threshold, 80);
    EXPECT_EQ(cfg.on_exceed, "block");
    EXPECT_TRUE(cfg.pr_comment);
    EXPECT_TRUE(cfg.ignore.empty());
    EXPECT_EQ(cfg.untagged_policy, "human");
    EXPECT_EQ(cfg.unverified_policy, "warn");
    EXPECT_TRUE(cfg.gitai_fallback);
    EXPECT_EQ(cfg.enforcement_scope, "final_diff");
    EXPECT_EQ(cfg.history_policy, "warn");
}

TEST(GhostConfig, LoadCustomConfig) {
    TempDir tmp;
    std::string configPath = tmp.path + "/ghost.yml";
    std::ofstream f(configPath);
    f << "version: 1\n";
    f << "required: true\n";
    f << "threshold: 50\n";
    f << "on_exceed: block\n";
    f << "pr_comment: true\n";
    f << "untagged_policy: block\n";
    f << "unverified_policy: ignore\n";
    f << "gitai_fallback: true\n";
    f.close();
    
    auto cfg = GhostConfigReader::load(tmp.path);
    
    EXPECT_EQ(cfg.version, 1);
    EXPECT_TRUE(cfg.required);
    EXPECT_EQ(cfg.threshold, 50);
    EXPECT_EQ(cfg.on_exceed, "block");
    EXPECT_TRUE(cfg.pr_comment);
    EXPECT_EQ(cfg.untagged_policy, "block");
    EXPECT_EQ(cfg.unverified_policy, "ignore");
    EXPECT_TRUE(cfg.gitai_fallback);
    EXPECT_EQ(cfg.enforcement_scope, "final_diff");
    EXPECT_EQ(cfg.history_policy, "warn");
}

TEST(GhostConfig, EnforcementBlock) {
    TempDir tmp;
    std::string configPath = tmp.path + "/ghost.yml";
    std::ofstream f(configPath);
    f << "enforcement:\n";
    f << "  scope: commit_history\n";
    f << "  history: block\n";
    f.close();

    auto cfg = GhostConfigReader::load(tmp.path);
    EXPECT_EQ(cfg.enforcement_scope, "commit_history");
    EXPECT_EQ(cfg.history_policy, "block");
}

TEST(GhostConfig, SaveAndLoad) {
    TempDir tmp;
    
    EXPECT_TRUE(GhostConfigReader::save(tmp.path, "threshold", "75"));
    
    auto cfg = GhostConfigReader::load(tmp.path);
    EXPECT_EQ(cfg.threshold, 75);
}

TEST(GhostConfig, GitAiFallbackAlias) {
    TempDir tmp;
    std::string configPath = tmp.path + "/ghost.yml";
    std::ofstream f(configPath);
    f << "gitai_fb: false\n";
    f.close();

    auto cfg = GhostConfigReader::load(tmp.path);
    EXPECT_FALSE(cfg.gitai_fallback);
}

TEST(GhostConfig, IgnorePaths) {
    TempDir tmp;
    std::string configPath = tmp.path + "/ghost.yml";
    std::ofstream f(configPath);
    f << "version: 1\n";
    f << "ignore:\n";
    f << "  - *.lock\n";
    f << "  - vendor/\n";
    f.close();
    
    auto cfg = GhostConfigReader::load(tmp.path);
    
    EXPECT_EQ(cfg.ignore.size(), 2);
    EXPECT_EQ(cfg.ignore[0], "*.lock");
    EXPECT_EQ(cfg.ignore[1], "vendor/");
}

TEST(GhostConfig, InvalidThreshold) {
    TempDir tmp;
    std::string configPath = tmp.path + "/ghost.yml";
    std::ofstream f(configPath);
    f << "threshold: not_a_number\n";
    f.close();
    
    auto cfg = GhostConfigReader::load(tmp.path);
    // Should fall back to default (80)
    EXPECT_EQ(cfg.threshold, 80);
}

TEST(GhostConfig, TrustedSigners) {
    TempDir tmp;
    std::string configPath = tmp.path + "/ghost.yml";
    std::ofstream f(configPath);
    f << "trusted_signers:\n";
    f << "  - name: Farhan\n";
    f << "    email: farhan@example.com\n";
    f << "    github: farhankhan197\n";
    f << "    ssh_key: ssh-ed25519 AAAATEST farhan@example.com\n";
    f.close();

    auto cfg = GhostConfigReader::load(tmp.path);
    ASSERT_EQ(cfg.trusted_signers.size(), 1);
    EXPECT_EQ(cfg.trusted_signers[0].name, "Farhan");
    EXPECT_EQ(cfg.trusted_signers[0].email, "farhan@example.com");
    EXPECT_EQ(cfg.trusted_signers[0].github, "farhankhan197");
    EXPECT_EQ(cfg.trusted_signers[0].ssh_key, "ssh-ed25519 AAAATEST farhan@example.com");
}
