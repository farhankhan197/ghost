#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static std::string runCaptureDoctor(const std::string& cmd, const std::string& cwd, int* exitCode = nullptr) {
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

static std::string ghostBinDoctor() {
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

static std::string envHomePrefixDoctor(const fs::path& home) {
#ifdef _WIN32
    return "set \"USERPROFILE=" + home.string() + "\" && set \"HOME=" + home.string() +
        "\" && set \"APPDATA=" + (home / "AppData" / "Roaming").string() + "\" && ";
#else
    return "USERPROFILE=\"" + home.string() + "\" HOME=\"" + home.string() + "\" ";
#endif
}

static void writeTextDoctor(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}

class DoctorRepo {
public:
    std::string path;

    DoctorRepo() {
        auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(rand());
        path = (fs::temp_directory_path() / ("ghost-doctor-test-" + suffix)).string();
        fs::create_directories(path);
        runCaptureDoctor("git init", path);
        runCaptureDoctor("git config user.name \"Doctor User\"", path);
        runCaptureDoctor("git config user.email \"doctor@example.com\"", path);
    }

    ~DoctorRepo() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

TEST(DoctorCli, DefaultHidesHealthyHookInternalsAndShowsRepair) {
    DoctorRepo repo;
    int rc = 0;
    std::string out = runCaptureDoctor("\"" + ghostBinDoctor() + "\" doctor", repo.path, &rc);

    EXPECT_NE(rc, 0);
    EXPECT_NE(out.find("ACTION NEEDED"), std::string::npos) << out;
    EXPECT_NE(out.find("repo hooks"), std::string::npos) << out;
    EXPECT_NE(out.find("ghost doctor --fix"), std::string::npos) << out;
    EXPECT_EQ(out.find("post-rewrite hook"), std::string::npos) << out;

    out = runCaptureDoctor("\"" + ghostBinDoctor() + "\" doctor --verbose", repo.path, &rc);
    EXPECT_NE(rc, 0);
    EXPECT_NE(out.find("post-rewrite hook"), std::string::npos) << out;
}

TEST(DoctorCli, WarnsWhenCodexGhostHooksNeedTrustRefresh) {
    DoctorRepo repo;
    fs::path home = fs::path(repo.path) / "home";
    fs::path codexDir = home / ".codex";
    fs::create_directories(codexDir);
    writeTextDoctor(codexDir / "hooks.json",
        "{\n"
        "  \"PreToolUse\": [{\"matcher\":\".*\",\"hooks\":[{\"type\":\"command\",\"command\":\"\\\"ghost\\\" pre --agent codex --hook-json\"}]}],\n"
        "  \"PostToolUse\": [{\"matcher\":\".*\",\"hooks\":[{\"type\":\"command\",\"command\":\"\\\"ghost\\\" post --agent codex --hook-json\"}]}]\n"
        "}\n");
    writeTextDoctor(codexDir / "config.toml",
        "[hooks.state.'codex:pre_tool_use:0:0']\n"
        "trusted_hash = \"old\"\n"
        "[hooks.state.'codex:post_tool_use:0:0']\n"
        "trusted_hash = \"old\"\n");

    auto now = fs::file_time_type::clock::now();
    std::error_code ec;
    fs::last_write_time(codexDir / "config.toml", now - std::chrono::hours(2), ec);
    fs::last_write_time(codexDir / "hooks.json", now, ec);

    int rc = 0;
    std::string out = runCaptureDoctor(
        envHomePrefixDoctor(home) + "\"" + ghostBinDoctor() + "\" doctor",
        repo.path,
        &rc);

    EXPECT_NE(rc, 0);
    EXPECT_NE(out.find("Codex hook trust"), std::string::npos) << out;
    EXPECT_NE(out.find("Ghost hooks changed after Codex last saved trusted hook state"), std::string::npos) << out;
    EXPECT_NE(out.find("/hooks"), std::string::npos) << out;
}
