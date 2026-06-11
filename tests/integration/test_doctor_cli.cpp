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
