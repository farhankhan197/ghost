#include "snapshot.hpp"
#include "checkpoint_store.hpp"
#include "util/process.hpp"
#include <filesystem>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace ghost {
namespace checkpoint {

std::vector<std::string> Snapshot::capture(const std::string& repoRoot) {
    CheckpointStore::ensureGhostDir(repoRoot);

    std::string ghostDir = CheckpointStore::getGhostDir(repoRoot);
    fs::path snapshotDir = fs::path(ghostDir) / "snapshot";
    std::error_code ec;

    if (fs::exists(snapshotDir, ec)) {
        for (const auto& entry : fs::directory_iterator(snapshotDir, ec)) {
            fs::remove_all(entry.path(), ec);
        }
    }
    fs::create_directories(snapshotDir, ec);

    std::string output = util::Process::capture("git ls-files --cached --others --exclude-standard");
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

bool Snapshot::captureSingle(const std::string& repoRoot, const std::string& filePath) {
    CheckpointStore::ensureGhostDir(repoRoot);

    std::string ghostDir = CheckpointStore::getGhostDir(repoRoot);
    fs::path snapshotDir = fs::path(ghostDir) / "snapshot";
    std::error_code ec;

    fs::create_directories(snapshotDir, ec);

    // Normalize filePath to relative if absolute
    fs::path relPath = filePath;
    if (relPath.is_absolute()) {
        relPath = fs::relative(relPath, repoRoot, ec);
        if (ec) return false;
    }

    fs::path srcPath = fs::path(repoRoot) / relPath;
    fs::path dstPath = snapshotDir / relPath;

    if (fs::exists(srcPath, ec) && fs::is_regular_file(srcPath, ec)) {
        fs::create_directories(dstPath.parent_path(), ec);
        fs::copy_file(srcPath, dstPath, fs::copy_options::overwrite_existing, ec);
        return !ec;
    }
    return false;
}

}
}
