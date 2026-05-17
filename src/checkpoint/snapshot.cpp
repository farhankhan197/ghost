#include "snapshot.hpp"
#include "working_log.hpp"
#include <cstdio>
#include <memory>
#include <filesystem>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace ghost {
namespace checkpoint {

static std::string runCommand(const std::string& cmd) {
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) return "";

    std::string result;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
        result += buffer;
    }

    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
        result.pop_back();
    }

    return result;
}

std::vector<std::string> Snapshot::capture(const std::string& repoRoot) {
    WorkingLog::ensureGhostDir(repoRoot);

    std::string ghostDir = WorkingLog::getGhostDir(repoRoot);
    fs::path snapshotDir = fs::path(ghostDir) / "snapshot";
    std::error_code ec;

    if (fs::exists(snapshotDir, ec)) {
        for (const auto& entry : fs::directory_iterator(snapshotDir, ec)) {
            fs::remove_all(entry.path(), ec);
        }
    }
    fs::create_directories(snapshotDir, ec);

    std::string output = runCommand("git diff --name-only");
    if (output.empty()) return {};

    std::vector<std::string> files;
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty()) continue;
        files.push_back(line);

        fs::path srcPath = fs::path(repoRoot) / line;
        fs::path dstPath = snapshotDir / line;

        if (fs::exists(srcPath, ec) && fs::is_regular_file(srcPath, ec)) {
            fs::create_directories(dstPath.parent_path(), ec);
            fs::copy_file(srcPath, dstPath, fs::copy_options::overwrite_existing, ec);
        }
    }

    return files;
}

}
}
