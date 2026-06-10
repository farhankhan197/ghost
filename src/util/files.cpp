#include "files.hpp"

#include <cstdlib>
#include <fstream>

namespace ghost {
namespace util {

bool Files::exists(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

std::string Files::homeDir() {
    const char* home = std::getenv("USERPROFILE");
    if (!home) home = std::getenv("HOME");
    return home ? home : "";
}

bool Files::writeTextIfMissing(const std::filesystem::path& path, const std::string& content, bool force) {
    std::error_code ec;
    if (!force && std::filesystem::exists(path, ec)) return true;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path);
    if (!file.is_open()) return false;
    file << content;
    return true;
}

bool Files::makeExecutable(const std::filesystem::path& path) {
    std::error_code ec;
#ifdef _WIN32
    return std::filesystem::exists(path, ec);
#else
    std::filesystem::permissions(
        path,
        std::filesystem::perms::owner_exec |
            std::filesystem::perms::group_exec |
            std::filesystem::perms::others_exec,
        std::filesystem::perm_options::add,
        ec
    );
    return !ec;
#endif
}

}
}
