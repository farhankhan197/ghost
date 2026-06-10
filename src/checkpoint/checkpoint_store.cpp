#include "checkpoint_store.hpp"
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace ghost {
namespace checkpoint {

std::string CheckpointStore::getGhostDir(const std::string& repoRoot) {
    return (fs::path(repoRoot) / ".git" / "ghost").string();
}

void CheckpointStore::ensureGhostDir(const std::string& repoRoot) {
    std::string ghostDir = getGhostDir(repoRoot);
    std::error_code ec;
    fs::create_directories(fs::path(ghostDir) / "snapshot", ec);
}

void CheckpointStore::clearSnapshot(const std::string& repoRoot) {
    std::string ghostDir = getGhostDir(repoRoot);
    std::error_code ec;

    fs::path snapshotDir = fs::path(ghostDir) / "snapshot";
    if (fs::exists(snapshotDir, ec)) {
        for (const auto& entry : fs::directory_iterator(snapshotDir, ec)) {
            fs::remove_all(entry.path(), ec);
        }
    }
}

}
}
