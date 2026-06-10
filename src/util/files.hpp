#ifndef GHOST_UTIL_FILES_HPP
#define GHOST_UTIL_FILES_HPP

#include <filesystem>
#include <string>

namespace ghost {
namespace util {

class Files {
public:
    static bool exists(const std::filesystem::path& path);
    static std::string homeDir();
    static std::string readText(const std::filesystem::path& path);
    static bool writeText(const std::filesystem::path& path, const std::string& content);
    static bool writeTextIfMissing(const std::filesystem::path& path, const std::string& content, bool force);
    static bool makeExecutable(const std::filesystem::path& path);
};

}
}

#endif
