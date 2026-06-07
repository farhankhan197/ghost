#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <memory>
#include <string>
#include <chrono>
#include <vector>

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

class CheckRepo {
public:
    std::string path;

    CheckRepo() {
        auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(rand());
        path = (fs::temp_directory_path() / ("ghost-check-test-" + suffix)).string();
        fs::create_directories(path);
        runCapture("git init", path);
        runCapture("git config user.name \"Check User\"", path);
        runCapture("git config user.email \"check@example.com\"", path);
        write("ghost.yml", "threshold: 100\nrequired: false\non_exceed: warn\n");
        commit("Initial policy");
    }

    ~CheckRepo() {
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

    void writeSession(const std::string& ranges) {
        fs::path sessionDir = fs::path(path) / ".git" / "ghost" / "sessions";
        fs::create_directories(sessionDir);
        std::ofstream f(sessionDir / "sess_check.json");
        f << "{\n"
          << "  \"session_id\": \"sess_check\",\n"
          << "  \"agent\": \"opencode\",\n"
          << "  \"model\": \"test-model\",\n"
          << "  \"author\": \"Check User <check@example.com>\",\n"
          << "  \"ts_start\": 1,\n"
          << "  \"ts_end\": 2,\n"
          << "  \"additions\": 99,\n"
          << "  \"deletions\": 0,\n"
          << "  \"entries\": [\n"
          << "    {\"file_path\": \"src/app.txt\", \"ranges\": \"" << ranges << "\"}\n"
          << "  ]\n"
          << "}\n";
    }
};

TEST(CheckCli, PredictsOnlyOverlappingStagedSessionRanges) {
    CheckRepo repo;
    repo.write("src/app.txt", "one\ntwo\nthree\n");
    repo.commit("Base file");

    repo.writeSession("4");
    repo.write("src/app.txt", "one\ntwo\nthree\nfour\nfive\n");
    runCapture("git add src/app.txt", repo.path);

    int rc = 0;
    std::string out = runCapture("\"" + ghostBin() + "\" check --json", repo.path, &rc);

    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("\"total_additions\": 2"), std::string::npos);
    EXPECT_NE(out.find("\"predicted_ai_additions\": 1"), std::string::npos);
}

TEST(CheckCli, DoesNotPredictAiForNonOverlappingSessionRanges) {
    CheckRepo repo;
    repo.write("src/app.txt", "one\ntwo\nthree\n");
    repo.commit("Base file");

    repo.writeSession("1-3");
    repo.write("src/app.txt", "one\ntwo\nthree\nfour\nfive\n");
    runCapture("git add src/app.txt", repo.path);

    int rc = 0;
    std::string out = runCapture("\"" + ghostBin() + "\" check --json", repo.path, &rc);

    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("\"total_additions\": 2"), std::string::npos);
    EXPECT_NE(out.find("\"predicted_ai_additions\": 0"), std::string::npos);
}
