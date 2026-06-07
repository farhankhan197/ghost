#include "path.hpp"
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace ghost {
namespace git {

static std::string slashNormalize(std::string value) {
    for (char& c : value) {
        if (c == '\\') c = '/';
    }
    while (value.rfind("./", 0) == 0) {
        value = value.substr(2);
    }
    return value;
}

NormalizedPath Path::normalizeRepoPath(const std::string& path, const std::string& repoRoot) {
    NormalizedPath result;
    if (path.empty()) return result;

    std::string input = path;
    if (input.rfind("a/", 0) == 0 || input.rfind("b/", 0) == 0) {
        input = input.substr(2);
    }

    fs::path p(input);
    if (!p.is_absolute()) {
        fs::path normalizedPath = p.lexically_normal();
        std::string rel = slashNormalize(normalizedPath.string());
        if (rel == "." || rel == ".." || rel.rfind("../", 0) == 0 || rel.find("/../") != std::string::npos) {
            return result;
        }
        result.path = rel;
        result.inside_repo = !result.path.empty();
        return result;
    }

    if (repoRoot.empty()) return result;

    std::error_code ec;
    fs::path rel = fs::relative(p, repoRoot, ec);
    if (ec || rel.empty()) return result;

    std::string relStr = slashNormalize(rel.string());
    if (relStr == "." || relStr.rfind("../", 0) == 0 || relStr == "..") return result;

    result.path = relStr;
    result.inside_repo = true;
    return result;
}

std::string Path::normalizeRepoPathOrEmpty(const std::string& path, const std::string& repoRoot) {
    auto normalized = normalizeRepoPath(path, repoRoot);
    return normalized.inside_repo ? normalized.path : "";
}

}
}
